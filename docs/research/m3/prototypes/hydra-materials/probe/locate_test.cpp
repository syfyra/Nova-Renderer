#include <pxr/pxr.h>
#include <pxr/base/arch/symbols.h>
#include <pxr/base/tf/fileUtils.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/stringUtils.h>
#include <MaterialXCore/Document.h>
#include <windows.h>
#include <cstdio>
PXR_NAMESPACE_USING_DIRECTIVE
int main() {
    MaterialX::createDocument(); // make sure the DLL is loaded
    std::string dll;
    void *base = nullptr;
    auto fn = &MaterialX::createDocument;
    std::printf("ArchGetAddressInfo: %d '%s'\n",
                ArchGetAddressInfo(reinterpret_cast<void *>(fn), &dll, &base, nullptr, nullptr), dll.c_str());
    if (HMODULE h = GetModuleHandleW(L"MaterialXCore.dll")) {
        wchar_t buf[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(h, buf, DWORD(std::size(buf)));
        std::string path(buf, buf + n); // ASCII install path assumed in this test
        const std::string libs = TfNormPath(TfGetPathName(path) + "../houdini/materialx/libraries");
        std::printf("MaterialXCore: %s\nlibraries: %s (exists=%d)\n", path.c_str(), libs.c_str(), int(TfIsDir(libs)));
    }
    return 0;
}
