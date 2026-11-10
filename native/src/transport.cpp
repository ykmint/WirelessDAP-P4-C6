#include "core.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>
using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;
static guid service_uuid{L"965a0001-0d7e-4e4d-9a5e-f742e87dc610"};
static guid rx_uuid{L"965a0002-0d7e-4e4d-9a5e-f742e87dc610"};
static guid tx_uuid{L"965a0003-0d7e-4e4d-9a5e-f742e87dc610"};
template<class T>auto await(T op){if(op.wait_for(std::chrono::seconds(12))==Windows::Foundation::AsyncStatus::Started){op.Cancel();throw std::runtime_error("Bluetooth operation timed out");}return op.GetResults();}
template<class T>auto ble_await(const char* stage,T op){try{return await(op);}catch(const winrt::hresult_error&e){throw std::runtime_error(std::string(stage)+": "+utf8(e.message().c_str()));}}
static Bytes frame(uint8_t op,uint32_t seq,std::span<const uint8_t>p){if(p.size()>256)throw std::runtime_error("Protocol payload too large");Bytes b{'W','D',1,op};put32(b,seq);put16(b,(uint16_t)p.size());b.insert(b.end(),p.begin(),p.end());return b;}
Bytes Link::request(uint8_t op,std::span<const uint8_t>p){std::lock_guard guard(mutex_);try{if(sequence_==UINT32_MAX)throw std::runtime_error("Session sequence exhausted");auto seq=++sequence_;auto reply=exchange(frame(op,seq,p));if(reply.size()<11||reply[0]!='W'||reply[1]!='D'||reply[2]!=1||reply[3]!=(op|128)||u32(reply.data()+4)!=seq||u16(reply.data()+8)!=reply.size()-10)throw std::runtime_error("Wireless response invalid; command not retried");switch(reply[10]){case 0:break;case 1:throw std::runtime_error("Access token mismatch or session expired");case 2:throw std::runtime_error("Device busy: disconnect another programmer first");case 3:throw std::runtime_error("Firmware rejected command or sequence: op="+std::to_string(op)+(op==2&&p.size()>=5?" DAP="+std::to_string(p[0])+" fields="+std::to_string(p[1])+","+std::to_string(p[2])+","+std::to_string(p[3])+","+std::to_string(p[4]):""));case 4:throw std::runtime_error("Target power fault: check VREF, external power and wiring");case 5:throw InterfaceError("Interface: "+std::string(reply.begin()+11,reply.end()));default:throw std::runtime_error("Unknown device error");}return Bytes(reply.begin()+11,reply.end());}catch(const InterfaceError&){throw;}catch(...){close();throw;}}
void Link::authenticate(const std::string&t){request(1,{(const uint8_t*)t.data(),t.size()});}
static void socket_io(SOCKET s,uint8_t*p,size_t n,bool write){while(n){int r=write?send(s,(char*)p,(int)n,0):recv(s,(char*)p,(int)n,0);if(r<=0)throw std::runtime_error("Wireless connection lost or timed out; programming outcome unverified");p+=r;n-=r;}}
class TCPLink final:public Link{
 SOCKET socket_=INVALID_SOCKET;
public:
 explicit TCPLink(const std::wstring&host,uint16_t port){ADDRINFOW hint{},*list=nullptr;hint.ai_family=AF_UNSPEC;hint.ai_socktype=SOCK_STREAM;hint.ai_protocol=IPPROTO_TCP;auto ps=std::to_wstring(port);if(GetAddrInfoW(host.c_str(),ps.c_str(),&hint,&list))throw std::runtime_error("IP address or hostname is invalid");
 for(auto p=list;p;p=p->ai_next){SOCKET s=socket(p->ai_family,p->ai_socktype,p->ai_protocol);if(s==INVALID_SOCKET)continue;u_long one=1;ioctlsocket(s,FIONBIO,&one);int r=connect(s,p->ai_addr,(int)p->ai_addrlen);if(r!=0&&WSAGetLastError()==WSAEWOULDBLOCK){fd_set set;FD_ZERO(&set);FD_SET(s,&set);timeval t{6,0};r=select(0,nullptr,&set,nullptr,&t)>0?0:-1;int err=0,len=sizeof(err);if(!r){getsockopt(s,SOL_SOCKET,SO_ERROR,(char*)&err,&len);if(err)r=-1;}}if(!r){one=0;ioctlsocket(s,FIONBIO,&one);DWORD timeout=10000;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeout,sizeof(timeout));setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(char*)&timeout,sizeof(timeout));BOOL nd=TRUE;setsockopt(s,IPPROTO_TCP,TCP_NODELAY,(char*)&nd,sizeof(nd));socket_=s;break;}closesocket(s);}
 FreeAddrInfoW(list);if(socket_==INVALID_SOCKET)throw std::runtime_error("Cannot reach ESP. Connect PC to WirelessDAP Wi-Fi, or enter its LAN IP.");}
 ~TCPLink(){close();}
 void close()noexcept override{if(socket_!=INVALID_SOCKET){shutdown(socket_,SD_BOTH);closesocket(socket_);socket_=INVALID_SOCKET;}}
 Bytes exchange(std::span<const uint8_t>b)override{if(socket_==INVALID_SOCKET)throw std::runtime_error("Not connected");socket_io(socket_,(uint8_t*)b.data(),b.size(),true);Bytes out(10);socket_io(socket_,out.data(),10,false);auto n=u16(out.data()+8);if(n>256||out[0]!='W'||out[1]!='D')throw std::runtime_error("Invalid device frame");out.resize(10+n);socket_io(socket_,out.data()+10,n,false);return out;}
};
std::shared_ptr<Link> connect_tcp(const std::wstring&h,uint16_t p,const std::string&t){auto l=std::make_shared<TCPLink>(h,p);l->authenticate(t);return l;}
std::vector<BleDevice> scan_ble(){
 BluetoothLEAdvertisementWatcher watcher;watcher.ScanningMode(BluetoothLEScanningMode::Active);struct State{std::mutex mutex;std::vector<BleDevice> found;BluetoothError error=BluetoothError::Success;};auto state=std::make_shared<State>();
 auto received=watcher.Received([state](auto const&,BluetoothLEAdvertisementReceivedEventArgs const&e){try{auto adv=e.Advertisement();auto name=std::wstring(adv.LocalName());bool matches=name.find(L"WirelessDAP")!=std::wstring::npos;for(auto const&uuid:adv.ServiceUuids())if(uuid==service_uuid)matches=true;if(matches){std::lock_guard guard(state->mutex);if(std::none_of(state->found.begin(),state->found.end(),[&](auto&d){return d.address==e.BluetoothAddress();}))state->found.push_back({e.BluetoothAddress(),name.empty()?L"WirelessDAP":name});}}catch(...){} });
 auto stopped=watcher.Stopped([state](auto const&,BluetoothLEAdvertisementWatcherStoppedEventArgs const&e){std::lock_guard guard(state->mutex);state->error=e.Error();});watcher.Start();std::this_thread::sleep_for(std::chrono::seconds(5));watcher.Stop();watcher.Received(received);watcher.Stopped(stopped);std::lock_guard guard(state->mutex);if(state->error!=BluetoothError::Success)throw std::runtime_error("Bluetooth scan unavailable. Turn on Bluetooth and allow Windows Bluetooth access.");return state->found;
}
class BLELink final:public Link{
 BluetoothLEPreferredConnectionParametersRequest throughput_{nullptr};
 BluetoothLEDevice device_{nullptr};GattSession session_{nullptr};GattDeviceService service_{nullptr};GattCharacteristic rx_{nullptr},tx_{nullptr};
public:
 explicit BLELink(uint64_t address){
  device_=ble_await("BLE open",BluetoothLEDevice::FromBluetoothAddressAsync(address));
  if(!device_){
   scan_ble();
   device_=ble_await("BLE open after scan",BluetoothLEDevice::FromBluetoothAddressAsync(address));
  }
  if(!device_)throw std::runtime_error("BLE device unavailable");
  session_=ble_await("BLE session",GattSession::FromDeviceIdAsync(device_.BluetoothDeviceId()));
  if(!session_)throw std::runtime_error("BLE session unavailable");
  session_.MaintainConnection(true);
  auto connectedDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
  while(session_.SessionStatus()!=GattSessionStatus::Active&&std::chrono::steady_clock::now()<connectedDeadline)
   std::this_thread::sleep_for(std::chrono::milliseconds(50));
  // Keep the interactive programmer responsive while this connection is open.
  // Windows 10 keeps its negotiated defaults; Windows 11 can request low latency.
  if(auto parameters=device_.try_as<IBluetoothLEDevice6>()){
   try{throughput_=parameters.RequestPreferredConnectionParameters(BluetoothLEPreferredConnectionParameters::ThroughputOptimized());}
   catch(const winrt::hresult_error&){OutputDebugStringW(L"WirelessDAP: BLE throughput preference unavailable\n");}
  }
  // Enumerate the complete service table: some controllers reject the ATT
  // Find By Type Value request used by filtered Windows service discovery.
  auto services=ble_await("BLE service discovery",device_.GetGattServicesAsync(BluetoothCacheMode::Uncached));
  if(services.Status()!=GattCommunicationStatus::Success)throw std::runtime_error("BLE service discovery failed");
  for(auto const& candidate:services.Services())if(candidate.Uuid()==service_uuid){service_=candidate;break;}
  if(!service_)throw std::runtime_error("WirelessDAP BLE service not found; check ESP firmware");
  auto chars=ble_await("BLE characteristic discovery",service_.GetCharacteristicsAsync(BluetoothCacheMode::Uncached));
  if(chars.Status()!=GattCommunicationStatus::Success)throw std::runtime_error("BLE characteristic discovery failed");
  for(auto const& c:chars.Characteristics()){if(c.Uuid()==rx_uuid)rx_=c;else if(c.Uuid()==tx_uuid)tx_=c;}
  if(!rx_||!tx_)throw std::runtime_error("BLE data characteristics unavailable");
 }
 ~BLELink(){close();}
 void close()noexcept override{try{if(throughput_)throughput_.Close();throughput_=nullptr;rx_=nullptr;tx_=nullptr;if(service_)service_.Close();service_=nullptr;if(session_){session_.MaintainConnection(false);session_.Close();}session_=nullptr;if(device_)device_.Close();device_=nullptr;}catch(...){} }
 Bytes exchange(std::span<const uint8_t> data)override{
 if(!device_)throw std::runtime_error("BLE disconnected");size_t chunk=20;
 if(session_){unsigned mtu=session_.MaxPduSize();if(mtu>23)chunk=std::min<size_t>(244,mtu-3);}
 for(size_t i=0;i<data.size();i+=chunk){DataWriter writer;auto size=std::min<size_t>(chunk,data.size()-i);writer.WriteBytes(array_view<const uint8_t>(data.data()+i,data.data()+i+size));auto result=ble_await("BLE write",rx_.WriteValueWithResultAsync(writer.DetachBuffer(),GattWriteOption::WriteWithResponse));if(result.Status()!=GattCommunicationStatus::Success)throw std::runtime_error("BLE write failed; command not retried");}
 auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);uint32_t seq=u32(data.data()+4);
 while(std::chrono::steady_clock::now()<until){auto result=ble_await("BLE read",tx_.ReadValueAsync(BluetoothCacheMode::Uncached));if(result.Status()!=GattCommunicationStatus::Success)throw std::runtime_error("BLE read failed");auto buffer=result.Value();Bytes b(buffer.Length());if(!b.empty()){DataReader reader=DataReader::FromBuffer(buffer);reader.ReadBytes(b);}if(b.size()>=10&&u32(b.data()+4)==seq)return b;std::this_thread::sleep_for(std::chrono::milliseconds(15));}throw std::runtime_error("BLE response timed out; command not retried");}
};
std::shared_ptr<Link> connect_ble(uint64_t a,const std::string&t){auto l=std::make_shared<BLELink>(a);l->authenticate(t);return l;}
Bridge::Bridge(std::shared_ptr<Link>l):link_(std::move(l)){
 listener_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);addr.sin_port=0;if(listener_==INVALID_SOCKET||bind(listener_,(sockaddr*)&addr,sizeof(addr))||listen(listener_,1)){if(listener_!=INVALID_SOCKET)closesocket(listener_);throw std::runtime_error("Local programming bridge unavailable");}int n=sizeof(addr);getsockname(listener_,(sockaddr*)&addr,&n);port=ntohs(addr.sin_port);thread_=std::thread([this]{serve();});}
Bridge::~Bridge(){stop_=true;auto s=client_.load();if(s!=INVALID_SOCKET)shutdown(s,SD_BOTH);if(thread_.joinable())thread_.join();closesocket(listener_);}
std::string Bridge::error(){std::lock_guard guard(error_mutex_);return error_;}
void Bridge::serve(){
 struct Apartment{Apartment(){winrt::init_apartment(winrt::apartment_type::multi_threaded);}~Apartment(){winrt::uninit_apartment();}} apartment;
 while(!stop_){fd_set set;FD_ZERO(&set);FD_SET(listener_,&set);timeval t{0,100000};if(select(0,&set,nullptr,nullptr,&t)<=0)continue;SOCKET s=accept(listener_,nullptr,nullptr);if(s==INVALID_SOCKET)continue;client_=s;DWORD timeout=15000;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeout,sizeof(timeout));BOOL nd=TRUE;setsockopt(s,IPPROTO_TCP,TCP_NODELAY,(char*)&nd,sizeof(nd));
 try{while(!stop_){uint8_t h[8];int first=recv(s,(char*)h,8,MSG_PEEK);if(first==0)break;if(first<0)throw std::runtime_error("OpenOCD bridge timed out");socket_io(s,h,8,false);uint16_t n=u16(h+4);if(u32(h)!=0x00504144||h[6]!=1||n==0||n>64)throw std::runtime_error("OpenOCD CMSIS-DAP packet rejected");Bytes p(64,0);socket_io(s,p.data(),n,false);
 // Bound firmware WAIT retries; never claim an unwired NRST can be asserted.
 if(p[0]==0x11){uint32_t hz=u32(p.data()+1);if(hz<10000||hz>100000){p[1]=0xa0;p[2]=0x86;p[3]=1;p[4]=0;}}
 if(p[0]==4){p[2]=std::min<uint16_t>(u16(p.data()+2),80);p[3]=0;p[4]=std::min<uint16_t>(u16(p.data()+4),80);p[5]=0;}
 if(p[0]==0x10&&(p[2]&128)&&!(p[1]&128))throw std::runtime_error("Target NRST is not wired; hardware reset unavailable");
 auto out=link_->request(2,p);if(out.empty()||out.size()>64)throw std::runtime_error("Invalid CMSIS-DAP response");Bytes hdr;put32(hdr,0x00504144);put16(hdr,(uint16_t)out.size());hdr.push_back(2);hdr.push_back(0);socket_io(s,hdr.data(),hdr.size(),true);socket_io(s,out.data(),out.size(),true);}}
 catch(const std::exception&e){if(!stop_){std::lock_guard guard(error_mutex_);error_=e.what();}}
 client_=INVALID_SOCKET;shutdown(s,SD_BOTH);closesocket(s);
 }
}


// USB serial uses a tagged ASCII envelope so boot/diagnostic log lines cannot
// be confused with WD frames. Inner framing/authentication is unchanged.
class USBLink final:public Link{
 HANDLE handle=INVALID_HANDLE_VALUE;
public:
 explicit USBLink(const std::wstring&port){
  if(port.size()<4||port.substr(0,3)!=L"COM"||port.find_first_not_of(L"0123456789",3)!=std::wstring::npos)throw std::runtime_error("Select a COM port");
  handle=CreateFileW((std::wstring(L"\\\\.\\")+port).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
  if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("USB port unavailable or occupied");
  DCB d{sizeof(d)};COMMTIMEOUTS t{MAXDWORD,0,100,0,2000};
  if(!GetCommState(handle,&d)){close();throw std::runtime_error("USB configuration read failed");}
  d.BaudRate=115200;d.ByteSize=8;d.Parity=NOPARITY;d.StopBits=ONESTOPBIT;d.fBinary=TRUE;d.fParity=FALSE;d.fOutxCtsFlow=FALSE;d.fOutxDsrFlow=FALSE;d.fDtrControl=DTR_CONTROL_DISABLE;d.fRtsControl=RTS_CONTROL_DISABLE;d.fOutX=FALSE;d.fInX=FALSE;
  if(!SetCommState(handle,&d)||!SetCommTimeouts(handle,&t)){close();throw std::runtime_error("USB configuration failed");}
 }
 ~USBLink(){close();}
 void close()noexcept override{if(handle!=INVALID_HANDLE_VALUE){CloseHandle(handle);handle=INVALID_HANDLE_VALUE;}}
 Bytes exchange(std::span<const uint8_t>b)override{
  std::string text="\nWDHEX:";const char*hex="0123456789ABCDEF";for(auto c:b){text+=hex[c>>4];text+=hex[c&15];}text+='\n';DWORD n=0;
  if(!WriteFile(handle,text.data(),(DWORD)text.size(),&n,nullptr)||n!=text.size())throw std::runtime_error("USB write failed; command not retried");
  std::string line;auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  auto digit=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;return -1;};
  while(std::chrono::steady_clock::now()<end){char c;DWORD got=0;if(!ReadFile(handle,&c,1,&got,nullptr))throw std::runtime_error("USB read failed");if(!got)continue;if(c=='\r')continue;if(c!='\n'){if(line.size()<8192)line+=c;else line.clear();continue;}
   auto tag=line.find("WDHEX:");if(tag!=std::string::npos)line.erase(0,tag);
   if(line.starts_with("WDHEX:")&&(line.size()-6)%2==0){Bytes out;bool valid=true;for(size_t i=6;i<line.size();i+=2){int a=digit(line[i]),v=digit(line[i+1]);if(a<0||v<0){valid=false;break;}out.push_back((a<<4)|v);}if(valid&&out.size()>=10&&u32(out.data()+4)==u32(b.data()+4))return out;}line.clear();
  }throw std::runtime_error("USB response timeout; command not retried");
 }
};
std::shared_ptr<Link> connect_usb(const std::wstring&p,const std::string&t){auto l=std::make_shared<USBLink>(p);l->authenticate(t);return l;}

void Link::disconnect() noexcept{try{request(23);}catch(...){}close();}

#include <setupapi.h>
#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"advapi32.lib")
std::vector<std::wstring> scan_usb(){
 const GUID portClass={0x4d36e978,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
 HDEVINFO devices=SetupDiGetClassDevsW(&portClass,nullptr,nullptr,DIGCF_PRESENT);std::vector<std::wstring> found;if(devices==INVALID_HANDLE_VALUE)return found;
 SP_DEVINFO_DATA d{sizeof(d)};
 for(DWORD i=0;SetupDiEnumDeviceInfo(devices,i,&d);i++){
  wchar_t hw[2048]{};if(!SetupDiGetDeviceRegistryPropertyW(devices,&d,SPDRP_HARDWAREID,nullptr,(BYTE*)hw,sizeof(hw),nullptr))continue;
  std::wstring id(hw);std::transform(id.begin(),id.end(),id.begin(),towupper);if(id.find(L"VID_303A")==id.npos)continue;
  HKEY key=SetupDiOpenDevRegKey(devices,&d,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_QUERY_VALUE);if(key==INVALID_HANDLE_VALUE)continue;
  wchar_t name[128]{};DWORD bytes=sizeof(name),type=0;auto rc=RegQueryValueExW(key,L"PortName",nullptr,&type,(BYTE*)name,&bytes);RegCloseKey(key);
  if(rc==ERROR_SUCCESS&&type==REG_SZ&&std::wstring(name).starts_with(L"COM"))found.emplace_back(name);
 }SetupDiDestroyDeviceInfoList(devices);return found;
}
