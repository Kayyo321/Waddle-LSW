"""Read bounded live Windows acceptance logs without denying their writers."""

import json
import re


def read_native_logs(session, guest_directory, patterns):
    """Return owned UTF-8 log strings from an existing guest directory.

    Args:
        session: Borrowed authenticated WinRM session, used synchronously.
        guest_directory: Borrowed absolute Windows directory string.
        patterns: Borrowed nonempty sequence of safe log filename patterns.

    Raises:
        ValueError: Invalid directory or filename pattern before guest access.
        RuntimeError: Native enumeration, sharing, decoding, or size failure.

    Each transient native stream/reader closes in finally. Readers permit the
    existing writer to remain open; individual logs are bounded to eight MiB.
    Caller serializes session access and decides where to retain the results.
    """
    if not isinstance(guest_directory, str) or not re.match(r"^[A-Za-z]:\\", guest_directory):
        raise ValueError("An absolute guest directory is required")
    if not patterns or any(not re.fullmatch(r"[A-Za-z0-9_.*?-]+\.log", item) for item in patterns):
        raise ValueError("Only safe log filename patterns are accepted")
    directory = guest_directory.replace("'", "''")
    filters = ",".join("'" + item + "'" for item in patterns)
    script = (
        "$ErrorActionPreference='Stop';"
        "function Read-LiveLog($path){$stream=$null;$reader=$null;try{"
        "$stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite);"
        "if($stream.Length -gt 8388608){throw 'Native log exceeds diagnostic bound'};"
        "$reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8,$true,1024,$true);$reader.ReadToEnd()"
        "}finally{if($reader){$reader.Dispose()};if($stream){$stream.Dispose()}}};"
        "$d='" + directory + "';$r=@{};foreach($filter in @(" + filters + ")){"
        "Get-ChildItem -LiteralPath $d -Filter $filter -File|ForEach-Object{"
        "$r[$_.Name]=Read-LiveLog $_.FullName}};$r|ConvertTo-Json -Compress"
    )
    result = session.run_ps(script)
    if result.status_code:
        raise RuntimeError("Native log collection failed: " + result.std_err.decode(errors="replace"))
    value = json.loads(result.std_out.decode("utf-8"))
    if not isinstance(value, dict) or any(not isinstance(item, str) for item in value.values()):
        raise RuntimeError("Invalid native log collection result")
    return value
