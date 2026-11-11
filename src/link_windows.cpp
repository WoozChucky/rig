// link_windows.cpp: finding Visual Studio and the Windows SDK, and
// link_executable on Windows (see link.h).
//
// Visual Studio 2017 and later don't register their directories anywhere
// simple. They answer through the Setup Configuration COM API, which lists
// the installed instances (Visual Studio, the Build Tools, and products
// that aren't compilers at all, like SQL Server Management Studio). Rig
// takes the newest instance that has the C++ build tools, which say their
// version in VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt. The
// SDK is simpler: the registry names its root, and its Lib directory has a
// directory per version.

#include "link.h"

#include <objbase.h>
#include <oleauto.h>

// The Setup Configuration API's interfaces, declared by hand from their
// documentation (Microsoft ships them as Setup.Configuration.h in a NuGet
// package). Only the methods Rig calls, and those before them: a COM
// interface is its methods' order.

struct ISetupInstance : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInstanceId(BSTR *id) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInstallDate(FILETIME *date) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInstallationName(BSTR *name) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInstallationPath(BSTR *path) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInstallationVersion(BSTR *version) = 0;
};

struct IEnumSetupInstances : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Next(ULONG count, ISetupInstance **instances, ULONG *fetched) = 0;
};

struct ISetupConfiguration : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EnumInstances(IEnumSetupInstances **instances) = 0;
};

static const CLSID CLSID_SetupConfiguration = {
    0x177F0C4A, 0x1CD3, 0x4DE7, {0xA3, 0x2C, 0x71, 0xDB, 0xBB, 0x9F, 0xA3, 0x6D}};
static const IID IID_ISetupConfiguration = {
    0x42843719, 0xDB4C, 0x46C2, {0x8E, 0x7C, 0x64, 0xF1, 0x81, 0x6E, 0xFD, 0x5B}};

static const char *INSTALL_BUILD_TOOLS =
    "install the Visual Studio Build Tools with the \"Desktop development with C++\" workload";

// Compares dotted version numbers ("17.14.36310.24", "10.0.26100.0") by
// their numbers, left to right: <0, 0 or >0.
static int compare_versions(String x, String y) {
    s64 i = 0, k = 0;
    while (i < x.count || k < y.count) {
        u64 a = 0, b = 0;
        while (i < x.count && x.data[i] >= '0' && x.data[i] <= '9') a = a * 10 + (x.data[i++] - '0');
        while (k < y.count && y.data[k] >= '0' && y.data[k] <= '9') b = b * 10 + (y.data[k++] - '0');
        if (a != b) return a < b ? -1 : 1;
        if (i < x.count) i++;  // the dot (or anything else between numbers)
        if (k < y.count) k++;
    }
    return 0;
}

static String trim(String s) {
    while (s.count > 0 && (s.data[s.count - 1] == '\r' || s.data[s.count - 1] == '\n' || s.data[s.count - 1] == ' ')) {
        s.count--;
    }
    while (s.count > 0 && (s.data[0] == ' ' || s.data[0] == '\t')) s = string_slice(s, 1, s.count - 1);
    return s;
}

// The installation directory of the newest Visual Studio instance with the
// C++ build tools, or {} when there's none.
static String find_visual_studio(Arena *a) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    String best_path = {}, best_version = {};
    ISetupConfiguration *config = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_SetupConfiguration, nullptr, CLSCTX_INPROC_SERVER, IID_ISetupConfiguration,
                                   (void **)&config))) {
        IEnumSetupInstances *instances = nullptr;
        if (SUCCEEDED(config->EnumInstances(&instances))) {
            ISetupInstance *instance = nullptr;
            ULONG fetched = 0;
            while (instances->Next(1, &instance, &fetched) == S_OK && fetched == 1) {
                BSTR path = nullptr, version = nullptr;
                if (SUCCEEDED(instance->GetInstallationPath(&path)) &&
                    SUCCEEDED(instance->GetInstallationVersion(&version))) {
                    String p = from_wide(a, path), v = from_wide(a, version);
                    String marker =
                        string_printf(a, STR_FMT "\\VC\\Auxiliary\\Build\\Microsoft.VCToolsVersion.default.txt", STR_ARG(p));
                    if (os_file_exists(marker) && (!best_path.count || compare_versions(v, best_version) > 0)) {
                        best_path = p;
                        best_version = v;
                    }
                }
                SysFreeString(path);
                SysFreeString(version);
                instance->Release();
            }
            instances->Release();
        }
        config->Release();
    }
    // RPC_E_CHANGED_MODE: someone initialised COM differently first; it
    // still works, and isn't ours to uninitialise.
    if (SUCCEEDED(init)) CoUninitialize();
    return best_path;
}

// The Windows SDK's root ("C:\Program Files (x86)\Windows Kits\10"), from
// either registry view, or {}.
static String find_kits_root(Arena *a) {
    REGSAM views[] = {KEY_WOW64_32KEY, KEY_WOW64_64KEY};
    for (REGSAM view : views) {
        HKEY key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots", 0,
                          KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) {
            continue;
        }
        DWORD type = 0, size = 0;
        String root = {};
        if (RegQueryValueExW(key, L"KitsRoot10", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_SZ) {
            wchar_t *value = (wchar_t *)arena_alloc(a, size + sizeof(wchar_t), 2);  // zeroed: terminated even if the value isn't
            if (RegQueryValueExW(key, L"KitsRoot10", nullptr, nullptr, (BYTE *)value, &size) == ERROR_SUCCESS) {
                root = from_wide(a, value);
            }
        }
        RegCloseKey(key);
        while (root.count > 0 && (root.data[root.count - 1] == '\\' || root.data[root.count - 1] == '/')) root.count--;
        if (root.count) return root;
    }
    return {};
}

bool find_windows_toolchain(Arena *a, Windows_Toolchain *out, String *error) {
    String vs = find_visual_studio(a);
    String version_text = {};
    if (!vs.count ||
        !os_read_file(a, string_printf(a, STR_FMT "\\VC\\Auxiliary\\Build\\Microsoft.VCToolsVersion.default.txt", STR_ARG(vs)),
                      &version_text)) {
        *error = string_printf(a, "Visual Studio with the C++ build tools wasn't found; %s", INSTALL_BUILD_TOOLS);
        return false;
    }
    String msvc = string_printf(a, STR_FMT "\\VC\\Tools\\MSVC\\" STR_FMT, STR_ARG(vs), STR_ARG(trim(version_text)));
    out->msvc_lib = string_printf(a, STR_FMT "\\lib\\x64", STR_ARG(msvc));
    String link_exe = string_printf(a, STR_FMT "\\bin\\Hostx64\\x64\\link.exe", STR_ARG(msvc));
    String lld_link = string_printf(a, STR_FMT "\\lld-link.exe", STR_ARG(os_executable_directory(a)));
    if (!choose_windows_linker(a, link_exe, lld_link, &out->link_exe, error)) return false;

    // The newest SDK version that has the x64 libraries.
    String kits = find_kits_root(a);
    String lib = string_printf(a, STR_FMT "\\Lib", STR_ARG(kits));
    Array<String> versions = {};
    String best = {};
    if (kits.count && os_list_directory(a, lib, &versions)) {
        for (s64 i = 0; i < versions.count; i++) {
            String kernel32 = string_printf(a, STR_FMT "\\" STR_FMT "\\um\\x64\\kernel32.lib", STR_ARG(lib), STR_ARG(versions[i]));
            if (os_file_exists(kernel32) && (!best.count || compare_versions(versions[i], best) > 0)) best = versions[i];
        }
    }
    array_free(&versions);
    if (!best.count) {
        *error = string_printf(a, "the Windows SDK wasn't found; %s", INSTALL_BUILD_TOOLS);
        return false;
    }
    out->ucrt_lib = string_printf(a, STR_FMT "\\" STR_FMT "\\ucrt\\x64", STR_ARG(lib), STR_ARG(best));
    out->um_lib = string_printf(a, STR_FMT "\\" STR_FMT "\\um\\x64", STR_ARG(lib), STR_ARG(best));
    return true;
}

bool link_executable(Arena *a, Target t, Array<String> objects, String output, String *error) {
    RIG_ASSERT(t == TARGET_WINDOWS_X64);  // no cross-linking yet
    Windows_Toolchain tc = {};
    if (!find_windows_toolchain(a, &tc, error)) return false;
    Array<String> argv = link_command_windows(a, &tc, objects, output);
    bool ok = run_linker(a, argv, error);
    array_free(&argv);
    return ok;
}
