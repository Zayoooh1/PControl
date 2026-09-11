#include "engine.hpp"
#include "ipc.hpp"
#include <sddl.h>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        auto name = L"Local\\PControl.Core." + pc::identity();
        pc::Handle mutex(CreateMutexW(nullptr, FALSE, name.c_str()));
        pc::check(bool(mutex), "Core mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS)
            return 0;
        pc::Engine engine(pc::dataDir());
        std::atomic_bool stop = false;
        pc::serve(
            stop,
            [&](const pc::Json &q) {
                if (q.value("version", 0) == 1 && q.value("command", "") == "Shutdown") {
                    stop = true;
                    return pc::success();
                }
                return engine.dispatch(q);
            },
            [&](const std::string &m) { engine.event(m); });
        return 0;
    } catch (const std::exception &e) {
        OutputDebugStringA(e.what());
        return 1;
    }
}
