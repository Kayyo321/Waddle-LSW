"""Root-reviewed private native run; existing base and legacy secrets untouched."""
import array, ast, collections, json, os, socket, struct, subprocess, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0,'/home/dev/Waddle-LSW/tests/interop')
from fresh_native_install import run_fresh_install, fresh_install_error_t
Repo=Path('/home/dev/Waddle-LSW')
Run=Repo/'build/fresh_native_guests/fresh-784f305e30df4aee9985169fafe7622b'
Access=Path(tempfile.mkdtemp(prefix='seamless-fresh-access-'))
# Extract only the already reviewed literal helper; do not import the old launcher.
module=ast.parse((Repo/'impl/seamless-interop/native_vsock_evidence_2026_10_24/launcher.py').read_text())
helper_code=next(ast.literal_eval(node.value) for node in module.body if isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='helper_code' for t in node.targets))
manifest=json.loads((Run/'run-manifest.json').read_text())
assert manifest['failure'] is None and manifest['guest_cid']==52943 and os.geteuid()==1001
listener=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
listener.bind(str(Access/'fd.sock'));listener.listen(1);listener.settimeout(10)
helper=subprocess.Popen(['sudo','-n','/usr/bin/python3','-I','-c',helper_code,str(Access/'fd.sock')],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,close_fds=True)
descriptors=[]
try:
 connection,_=listener.accept()
 with connection:
  connection.settimeout(5)
  _,uid,_=struct.unpack('3i',connection.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12))
  if uid!=0:raise RuntimeError('device helper identity mismatch')
  data,ancillary,flags,_=connection.recvmsg(1,socket.CMSG_SPACE(array.array('i').itemsize),socket.MSG_CMSG_CLOEXEC)
  for level,kind,payload in ancillary:
   if level==socket.SOL_SOCKET and kind==socket.SCM_RIGHTS:
    part=array.array('i');part.frombytes(payload);descriptors.extend(part)
  if data!=b'V' or flags&socket.MSG_CTRUNC or len(descriptors)!=1:raise RuntimeError('device descriptor handoff failed')
 helper.wait(timeout=10)
 if helper.returncode!=0:raise RuntimeError('device helper failed')
finally:
 listener.close();(Access/'fd.sock').unlink(missing_ok=True)
 if helper.poll() is None:helper.kill();helper.wait(timeout=5)
os.mkfifo(Run/'stop.fifo',0o600)
stop_read=os.open(Run/'stop.fifo',os.O_RDWR|os.O_NONBLOCK|os.O_CLOEXEC)
stop_write=stop_read
stop=threading.Event()
receipt={'guest_cid':52943,'port':5242,'run_root':str(Run),'uid':os.geteuid(),'attempts':0,'errors':{},'reachable':False,'execution':None,'native_interop_acceptance':False}
errors=collections.Counter()
def probe():
 deadline=time.monotonic()+1810
 while not stop.is_set() and time.monotonic()<deadline:
  peer=socket.socket(socket.AF_VSOCK,socket.SOCK_STREAM);peer.settimeout(1)
  try:
   receipt['attempts']+=1;peer.connect((52943,5242));receipt['reachable']=True;peer.close()
   result=subprocess.run([str(Repo/'build/waddle'),'exec','--vsock-cid','52943','--vsock-port','5242','--pipe','--cwd','C:\\','--timeout','10','--','cmd.exe','/c','ver & whoami'],stdin=subprocess.DEVNULL,capture_output=True,timeout=15)
   receipt['execution']={'exit_code':result.returncode,'stdout':result.stdout[:4096].decode(errors='replace'),'stderr':result.stderr[:4096].decode(errors='replace')}
   if result.returncode==0:os.write(stop_write,b'stop');return
  except OSError as error:errors[str(error.errno)]+=1
  except subprocess.TimeoutExpired:receipt['execution']={'timeout':True}
  finally:peer.close()
  stop.wait(3)
worker=threading.Thread(target=probe,name='fresh-native-bounded-probe')
try:
 worker.start()
 outcome=run_fresh_install(Run,manifest,descriptors[0],stop_fd=stop_read,screenshot_interval_seconds=30,install_boot_key=True)
 receipt['supervisor']={k:outcome.get(k) for k in ('failure','stop_requested','deadline_reached','qemu_reaped','swtpm_reaped','forced_qemu_termination')}
except BaseException as error:
 receipt['supervisor_failure']=type(error).__name__
finally:
 stop.set();worker.join(timeout=20)
 receipt['probe_joined']=not worker.is_alive();receipt['errors']=dict(errors)
 for fd in set(descriptors+[stop_read,stop_write]):os.close(fd)
 output=Run/'guest-exec-probe.json'
 fd=os.open(output,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
 with os.fdopen(fd,'w') as sink:json.dump(receipt,sink,indent=2);sink.write('\n')
 print(json.dumps({'receipt':str(output),**receipt},indent=2),flush=True)
