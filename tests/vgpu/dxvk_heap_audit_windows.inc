/** @file dxvk_heap_audit_windows.inc @brief Optional native UMDH stage gates.
 * The supervisor owns a fresh absolute directory and each acknowledgement.
 * This fixture owns module references and marker handles, closing every marker
 * before announcing a quiescent stage. No heap allocation count is accepted as
 * leak proof; the supervisor must collect and attribute UMDH allocation stacks.
 */
/** @brief Copy a bounded optional absolute audit directory from the environment.
 * @param[out] path Nonnull caller-owned array of capacity wchar_t elements.
 * @param[in] capacity Array extent. @return 0 absent, 1 valid, -1 malformed.
 * @note Sole main thread. No allocation or retained environment pointer.
 */
static int audit_directory(wchar_t *path,DWORD capacity)
{
    SetLastError(ERROR_SUCCESS);
    DWORD length=GetEnvironmentVariableW(L"WADDLE_DXVK_HEAP_AUDIT",path,capacity);
    if(!length)return GetLastError()==ERROR_ENVVAR_NOT_FOUND ? 0 : -1;
    if(length>=capacity || length<3 || path[1]!=L':' || path[2]!=L'\\')return -1;
    DWORD attributes=GetFileAttributesW(path);
    return attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY) &&
        !(attributes&FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : -1;
}
/** @brief Pause at a closed-handle native heap capture point.
 * @param[in] directory Borrowed absolute existing supervisor directory.
 * @param[in] stage Borrowed trusted ASCII constant without path separators.
 * @return Zero only after an exact regular-file acknowledgement; one on error.
 * @note Sole main thread. Owns ready/ack file handles only until CloseHandle.
 * Wait is bounded to 60 seconds. All GPU/controller deadlines remain unchanged.
 */
static int audit_stage(const wchar_t *directory,const char *stage)
{
    wchar_t ready[1024],ack[1024],wide_stage[64];
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,stage,-1,wide_stage,64))return 1;
    if(swprintf(ready,1024,L"%ls\\%ls.ready",directory,wide_stage)<0 ||
       swprintf(ack,1024,L"%ls\\%ls.continue",directory,wide_stage)<0)return 1;
    if(GetFileAttributesW(ack)!=INVALID_FILE_ATTRIBUTES)return 1;
    FILETIME created,exited,kernel,user;DWORD handles=0;
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user) ||
       !GetProcessHandleCount(GetCurrentProcess(),&handles))return 1;
    uint64_t identity=((uint64_t)created.dwHighDateTime<<32)|created.dwLowDateTime;
    char record[256];int length=snprintf(record,sizeof record,"pid=%lu creation=%016llx handles=%lu stage=%s\n",
        (unsigned long)GetCurrentProcessId(),(unsigned long long)identity,(unsigned long)handles,stage);
    if(length<=0 || (size_t)length>=sizeof record)return 1;
    HANDLE file=CreateFileW(ready,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE)return 1;
    DWORD written=0;int failed=!WriteFile(file,record,(DWORD)length,&written,NULL) || written!=(DWORD)length;
    if(!CloseHandle(file))failed=1;
    if(failed)return 1;
    printf("DXVK heap capture %s pid=%lu creation=%016llx handles=%lu\n",stage,
        (unsigned long)GetCurrentProcessId(),(unsigned long long)identity,(unsigned long)handles);fflush(stdout);
    char expected[80];length=snprintf(expected,sizeof expected,"continue=%s\n",stage);
    if(length<=0 || (size_t)length>=sizeof expected)return 1;
    ULONGLONG started=GetTickCount64();
    do {
        file=CreateFileW(ack,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        if(file!=INVALID_HANDLE_VALUE){
            BY_HANDLE_FILE_INFORMATION information={0};char actual[81];DWORD count=0;
            int match=GetFileType(file)==FILE_TYPE_DISK && GetFileInformationByHandle(file,&information) &&
                !(information.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) &&
                information.nNumberOfLinks==1 && !information.nFileSizeHigh && information.nFileSizeLow==(DWORD)length &&
                ReadFile(file,actual,sizeof actual,&count,NULL) && count==(DWORD)length && !memcmp(actual,expected,(size_t)length);
            if(!CloseHandle(file))return 1;
            return match ? 0 : 1;
        }
        if(GetLastError()!=ERROR_FILE_NOT_FOUND)return 1;
        Sleep(10);
    }while(GetTickCount64()-started<60000);
    fputs("Native heap stage acknowledgement timed out.\n",stderr);return 1;
}
