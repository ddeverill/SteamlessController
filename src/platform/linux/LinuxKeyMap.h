#pragma once
#include <cstdint>

// Translates the app's canonical key id (core/KeyNames.h — numerically a
// Windows VK code) to a Linux evdev KEY_* code. Returns 0 for a key id with
// no mapping (mirrors VkFromJsCode's own "do nothing" contract for anything
// unrecognised — some VK codes, notably the layout-dependent VK_OEM_* ones,
// have no clean evdev counterpart and are left unmapped rather than guessed at).
uint16_t EvdevKeyFromKeyId(uint16_t keyId);
