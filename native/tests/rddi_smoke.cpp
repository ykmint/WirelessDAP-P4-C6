#include <cstdio>
#include <vector>
#include <windows.h>
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  HMODULE dll = LoadLibraryA(argv[1]);
  if (!dll) {
    std::printf("LoadLibrary error %lu\n", GetLastError());
    return 3;
  }
  auto open = reinterpret_cast<int (*)(int *, const void *)>(
      GetProcAddress(dll, "RDDI_Open"));
  auto close =
      reinterpret_cast<int (*)(int)>(GetProcAddress(dll, "RDDI_Close"));
  auto read = reinterpret_cast<int (*)(int, int, int, int *)>(
      GetProcAddress(dll, "DAP_ReadReg"));
  auto repeat = reinterpret_cast<int (*)(int, int, int, int, int *)>(
      GetProcAddress(dll, "DAP_RegReadRepeat"));
  if (!open || !close || !read || !repeat)
    return 4;
  int handle = 0, value = 0;
  int code = open(&handle, nullptr);
  std::printf("RDDI_Open=%d\n", code);
  if (code)
    return 5;
  code = read(handle, 0, 0, &value);
  std::printf("DAP_ReadReg=%d DP=%08X\n", code, value);
  if (code || value != 0x6ba02477) {
    close(handle);
    return 6;
  }
  std::vector<int> values(350);
  code = repeat(handle, 0, (int)values.size(), 0, values.data());
  bool equal = true;
  for (int item : values)
    if (item != value)
      equal = false;
  std::printf("DAP_RegReadRepeat=%d count=350 equal=%d\n", code, equal);
  close(handle);
  FreeLibrary(dll);
  return code || !equal ? 7 : 0;
}
