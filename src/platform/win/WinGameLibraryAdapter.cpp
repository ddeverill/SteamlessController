#include "WinGameLibraryAdapter.h"
#include "WinGameLibrary.h"
#include "core/Text.h"

std::vector<InstalledGame> WinGameLibraryAdapter::EnumerateInstalled() {
    std::vector<InstalledGame> out;
    for (auto& g : GameLibrary::EnumerateInstalled())
        out.push_back({ WideToUtf8(g.name), WideToUtf8(g.id) });
    return out;
}
