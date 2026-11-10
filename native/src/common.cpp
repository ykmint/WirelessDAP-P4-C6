#include "core.h"
#include <compressapi.h>
#include <bcrypt.h>
#include <charconv>
#pragma comment(lib,"cabinet.lib")
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"ws2_32.lib")
std::wstring wide(std::string_view s){if(s.empty())return {};int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0);if(!n)return L"无法解码文本";std::wstring r(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n);return r;}
std::string utf8(std::wstring_view s){if(s.empty())return {};int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);std::string r(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr);return r;}
Bytes read_file(const fs::path&p){std::ifstream f(p,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Cannot open file: "+utf8(p.wstring()));auto n=f.tellg();if(n<0||n>512*1024*1024)throw std::runtime_error("File size exceeds limit");Bytes b((size_t)n);f.seekg(0);if(n&&!f.read((char*)b.data(),n))throw std::runtime_error("File read failed");return b;}
void write_file(const fs::path&p,std::span<const uint8_t>b){fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary|std::ios::trunc);if(!f||!f.write((const char*)b.data(),b.size()))throw std::runtime_error("File write failed: "+utf8(p.wstring()));}
uint16_t u16(const uint8_t*p){return uint16_t(p[0])|uint16_t(p[1])<<8;}
uint32_t u32(const uint8_t*p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
void put16(Bytes&b,uint16_t v){b.push_back(v&255);b.push_back(v>>8);}
void put32(Bytes&b,uint32_t v){for(int i=0;i<4;i++)b.push_back((v>>(8*i))&255);}
std::wstring hex32(uint32_t v){std::wstringstream s;s<<L"0x"<<std::hex<<std::uppercase<<std::setw(8)<<std::setfill(L'0')<<v;return s.str();}
int json_int(std::string_view s,std::string_view key){auto pos=s.find("\""+std::string(key)+"\"");if(pos==s.npos)throw std::runtime_error("Missing device status field");pos=s.find(':',pos);if(pos==s.npos)throw std::runtime_error("Malformed device status");pos++;while(pos<s.size()&&s[pos]==' ')pos++;int v=0;auto result=std::from_chars(s.data()+pos,s.data()+s.size(),v);if(result.ec!=std::errc())throw std::runtime_error("Malformed numeric device status");return v;}
static std::string digest(std::span<const uint8_t>bytes){uint8_t hash[32];NTSTATUS n=BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,(PUCHAR)bytes.data(),(ULONG)bytes.size(),hash,32);if(n<0)throw std::runtime_error("SHA256 failed");std::ostringstream out;for(auto c:hash)out<<std::hex<<std::setw(2)<<std::setfill('0')<<int(c);return out.str();}
fs::path unpack_assets(){
 HRSRC r=FindResourceW(nullptr,MAKEINTRESOURCEW(101),RT_RCDATA);if(!r)throw std::runtime_error("Embedded engine missing");HGLOBAL h=LoadResource(nullptr,r);auto data=(uint8_t*)LockResource(h);size_t n=SizeofResource(nullptr,r);if(n<16)throw std::runtime_error("Embedded engine invalid");
 uint32_t size=u32(data+4);if(u32(data)!=0x32424457||size>128*1024*1024)throw std::runtime_error("Embedded engine header invalid");
 DECOMPRESSOR_HANDLE dc=nullptr;if(!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&dc))throw std::runtime_error("Windows decompressor unavailable");Bytes raw(size);SIZE_T actual=0;BOOL ok=Decompress(dc,data+8,n-8,raw.data(),raw.size(),&actual);CloseDecompressor(dc);if(!ok||actual!=size)throw std::runtime_error("Embedded engine decompression failed");
 wchar_t temp[32768];if(!GetTempPathW(32768,temp))throw std::runtime_error("Temporary directory unavailable");fs::path root=fs::path(temp)/L"WirelessDAP"/wide(digest({data,n}).substr(0,16));fs::create_directories(root);
 size_t off=0;auto take=[&](size_t len){if(len>raw.size()-off)throw std::runtime_error("Truncated embedded engine");auto p=raw.data()+off;off+=len;return p;};uint32_t count=u32(take(4));if(count>10000)throw std::runtime_error("Invalid embedded file count");
 for(uint32_t i=0;i<count;i++){uint16_t pn=u16(take(2));uint32_t len=u32(take(4));std::string name((char*)take(pn),pn);auto bytes=take(len);fs::path rel=wide(name);if(rel.is_absolute()||name.find("..")!=std::string::npos)throw std::runtime_error("Unsafe embedded path");auto dest=root/rel;bool valid=false;try{valid=read_file(dest)==Bytes(bytes,bytes+len);}catch(...){}if(!valid)write_file(dest,{bytes,len});}
 if(off!=raw.size())throw std::runtime_error("Trailing embedded data");return root;
}
int run_process(const fs::path&exe,const std::wstring&args,const fs::path&cwd,Log log,unsigned timeout,std::function<void()> tick){
 SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE rd=nullptr,wr=nullptr;if(!CreatePipe(&rd,&wr,&sa,0))throw std::runtime_error("Cannot create log pipe");SetHandleInformation(rd,HANDLE_FLAG_INHERIT,0);
 STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;si.hStdOutput=wr;si.hStdError=wr;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);PROCESS_INFORMATION pi{};
 std::wstring command=L"\""+exe.wstring()+L"\" "+args;BOOL ok=CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,cwd.c_str(),&si,&pi);CloseHandle(wr);if(!ok){CloseHandle(rd);throw std::runtime_error("Cannot start embedded OpenOCD, Windows error "+std::to_string(GetLastError()));}
 HANDLE job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji{};ji.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;if(job){SetInformationJobObject(job,JobObjectExtendedLimitInformation,&ji,sizeof(ji));AssignProcessToJobObject(job,pi.hProcess);}
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(timeout);std::string pending;bool expired=false;std::exception_ptr failure;auto tickAt=std::chrono::steady_clock::now();
 for(;;){if(tick&&std::chrono::steady_clock::now()>=tickAt){try{tick();}catch(...){failure=std::current_exception();TerminateProcess(pi.hProcess,125);break;}tickAt=std::chrono::steady_clock::now()+std::chrono::milliseconds(50);}DWORD available=0;if(PeekNamedPipe(rd,nullptr,0,nullptr,&available,nullptr)&&available){char buf[4096];DWORD got=0;if(ReadFile(rd,buf,std::min<DWORD>(available,sizeof(buf)),&got,nullptr)&&got){pending.append(buf,got);size_t end;while((end=pending.find('\n'))!=std::string::npos){std::string line=pending.substr(0,end);pending.erase(0,end+1);if(!line.empty()&&line.back()=='\r')line.pop_back();if(!line.empty())log(wide(line));}}}
 else if(WaitForSingleObject(pi.hProcess,20)==WAIT_OBJECT_0){DWORD tail=0;if(PeekNamedPipe(rd,nullptr,0,nullptr,&tail,nullptr)&&tail)continue;break;}
 if(std::chrono::steady_clock::now()>deadline){TerminateProcess(pi.hProcess,124);expired=true;break;}}
 if(!pending.empty())log(wide(pending));DWORD code=1;WaitForSingleObject(pi.hProcess,2000);GetExitCodeProcess(pi.hProcess,&code);CloseHandle(rd);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);if(job)CloseHandle(job);if(failure)std::rethrow_exception(failure);if(expired)throw std::runtime_error("Programming engine timed out; result unverified");return int(code);
}
