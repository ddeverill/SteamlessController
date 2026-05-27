#include "ControllerManager.h"
#include "VirtualController.h"
#include "steam/SteamController.h"
#include <memory>

static std::unique_ptr<SteamController> g_ctrl;

ControllerManager::ControllerManager(StateChangedFn onStateChanged)
    : m_onStateChanged(std::move(onStateChanged))
{
    TryOpen();
}

ControllerManager::~ControllerManager() {
    Close(/*restoreLizard=*/true);
}

void ControllerManager::OnDeviceChange() {
    if (!m_connected)
        TryOpen();
    else if (g_ctrl && !g_ctrl->IsOpen())
        Close(/*restoreLizard=*/false);
}

void ControllerManager::EnableGameMode() {
    if (!m_connected || m_gameModeActive) return;
    if (!g_ctrl->DisableLizardMode()) return;

    m_virtual = std::make_unique<VirtualController>(
        [](uint8_t largeMotor, uint8_t smallMotor) {
            if (!g_ctrl || !g_ctrl->IsOpen()) return;

            const uint16_t leftSpeed  = static_cast<uint16_t>(static_cast<uint32_t>(largeMotor) * 257u);
            const uint16_t rightSpeed = static_cast<uint16_t>(static_cast<uint32_t>(smallMotor) * 257u);
            g_ctrl->SetRumble(leftSpeed, rightSpeed);
        });
    if (!m_virtual->IsValid()) {
        bool missing = m_virtual->IsDriverMissing();
        m_virtual.reset();
        g_ctrl->EnableLizardMode();
        if (missing) m_onStateChanged(m_connected, m_gameModeActive, /*vigemMissing=*/true);
        return;
    }

    m_gameModeActive = true;
    m_trackpad.Reset();
    m_trackpad.SetTrackpadEnabled(m_trackpadMouseEnabled);
    m_trackpad.SetBackButtonsEnabled(m_backButtonsEnabled);
    m_trackpad.SetUseLeftTrackpad(m_useLeftTrackpad);
    StartReadLoop();
    m_onStateChanged(m_connected, m_gameModeActive, false);
}

void ControllerManager::DisableGameMode() {
    if (!m_gameModeActive) return;
    StopReadLoop();
    if (g_ctrl && g_ctrl->IsOpen())
        g_ctrl->SetRumble(0, 0);
    m_trackpad.Reset();
    m_virtual.reset();
    g_ctrl->EnableLizardMode();
    m_gameModeActive = false;
    m_onStateChanged(m_connected, m_gameModeActive, false);
}

void ControllerManager::SetTrackpadMouseEnabled(bool enabled) {
    m_trackpadMouseEnabled = enabled;
    m_trackpad.SetTrackpadEnabled(enabled);
}

void ControllerManager::SetBackButtonsEnabled(bool enabled) {
    m_backButtonsEnabled = enabled;
    m_trackpad.SetBackButtonsEnabled(enabled);
}

void ControllerManager::SetUseLeftTrackpad(bool enabled) {
    m_useLeftTrackpad = enabled;
    m_trackpad.SetUseLeftTrackpad(enabled);
}

void ControllerManager::TryOpen() {
    if (!g_ctrl) g_ctrl = std::make_unique<SteamController>();
    if (g_ctrl->Open()) {
        m_connected = true;
        m_onStateChanged(m_connected, m_gameModeActive, false);
    }
}

void ControllerManager::Close(bool restoreLizard) {
    StopReadLoop();
    m_virtual.reset();
    if (g_ctrl) {
        if (restoreLizard && m_gameModeActive)
            g_ctrl->EnableLizardMode();
        g_ctrl->Close();
    }
    m_connected      = false;
    m_gameModeActive = false;
    m_onStateChanged(m_connected, m_gameModeActive, false);
}

void ControllerManager::StartReadLoop() {
    m_readRunning = true;
    m_readThread  = std::thread(&ControllerManager::ReadLoop, this);
}

void ControllerManager::StopReadLoop() {
    m_readRunning = false;
    if (m_readThread.joinable())
        m_readThread.join();
}

void ControllerManager::ReadLoop() {
    uint8_t buf[64];
    while (m_readRunning) {
        size_t n = g_ctrl->ReadReport(buf, sizeof(buf), /*timeoutMs=*/32);
        if (n == 0) continue;
        if (buf[0] != SteamController::REPORT_STATE) continue;
        if (m_virtual) m_virtual->Update(buf, n);
        m_trackpad.Update(buf, n);
    }
}
