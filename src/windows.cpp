#include "includes.hpp"

#include <cstdint>
#include <atomic>

LARGE_INTEGER freq;

constexpr size_t RING_BUFFER_SIZE = 256;

struct __attribute__((packed)) SharedMemory {
	volatile uint32_t head;
	volatile uint32_t tail;
	volatile uint32_t error_flag;
	volatile uint32_t heartbeat;
	LinuxInputEvent events[RING_BUFFER_SIZE];
};

HANDLE hShmFile = NULL;
HANDLE hShmMapping = NULL;
SharedMemory* pSharedMem = nullptr;

// notify the player if theres an issue with input on Linux
#include <Geode/modify/CreatorLayer.hpp>
class $modify(CreatorLayer) {
	bool init() {
		if (!CreatorLayer::init()) return false;

		if (linuxNative && pSharedMem && pSharedMem->error_flag == 3 && !softToggle) {
			log::error("Linux input failed");
			FLAlertLayer* popup = FLAlertLayer::create(
				"CBF Linux",
				"Failed to read input devices.\nOn most distributions, this can be resolved with the following command: <cr>sudo usermod -aG input $USER</c> (reboot afterward; this will make your system slightly less secure).\nIf the issue persists, please contact the mod developer.",
				"OK"
			);
			popup->m_scene = this;
			popup->show();
		}
		return true;
	}
};

// ---- SOCD compatibility (Linux/Wine keyboard path) ----
// Keyboards with SOCD cleaning ("last input priority") answer "press key2 while key1 is held" with
// "release key1" + "press key2" (in either order, within ~1ms). Vanilla GD collapses those into one frame,
// but CBF replays every event with its own timestamp, so the release really lets go of the button.
// Fix: (1) only report a press/release when the *action* (not the key) changes state, and
// (2) hold back a release for a moment; if a press of the same action arrives inside the window, drop both.
bool socdFix = false;
constexpr double SOCD_WINDOW = 0.002; // seconds

struct PendingRelease {
	bool active = false;
	PlayerButtonCommand cmd;
};
static std::array<std::unordered_set<USHORT>, 6> heldKeys;   // physical keys currently down, per action
static std::array<PendingRelease, 6> pendingReleases;

void linuxResetKeyState() {
	for (auto& s : heldKeys) s.clear();
	for (auto& p : pendingReleases) p.active = false;
}

// emit held-back releases that are older than SOCD_WINDOW relative to `now`
static void flushPendingReleases(double now) {
	for (auto& p : pendingReleases) {
		if (p.active && p.cmd.m_timestamp + SOCD_WINDOW < now) {
			inputVector.emplace_back(p.cmd);
			p.active = false;
		}
	}
}

void linuxHeartbeat() {
	if (pSharedMem) pSharedMem->heartbeat++;
}

void linuxCheckInputs() {
	if (!pSharedMem) return;

	static std::unordered_map<int, enumKeyCodes> linuxToCCKey = {
		{ BTN_A, CONTROLLER_A },
		{ BTN_B, CONTROLLER_B },
		{ BTN_X, CONTROLLER_X },
		{ BTN_Y, CONTROLLER_Y },
		{ BTN_TL, CONTROLLER_LB },
		{ BTN_TR, CONTROLLER_RB },
		{ BTN_SELECT, CONTROLLER_Back },
		{ BTN_START, CONTROLLER_Start },
	};

	uint32_t h = pSharedMem->head;
	std::atomic_thread_fence(std::memory_order_acquire);
	uint32_t t = pSharedMem->tail;

	while (t != h) {
		const LinuxInputEvent& ev = pSharedMem->events[t & (RING_BUFFER_SIZE - 1)];
		t++;

		PlayerButtonCommand input;
		bool player1 = true;
		USHORT scanCode = ev.code;
		int value = ev.value;

		switch (ev.deviceType) {
		case MOUSE:
		case TOUCHPAD:
			if (scanCode == BUTTON_LEFT) {
				input.m_button = PlayerButton::Jump;
			}
			else if (scanCode == BUTTON_RIGHT) {
				if (!enableRightClick) continue;
				input.m_button = PlayerButton::Jump;
				player1 = false;
			}
			break;
		case KEYBOARD: {
			USHORT keyCode = MapVirtualKeyExA(scanCode, MAPVK_VSC_TO_VK, GetKeyboardLayout(0));
			GameAction action;
			if (inputBinds[p1Jump].contains(keyCode)) action = p1Jump;
			else if (inputBinds[p1Left].contains(keyCode)) action = p1Left;
			else if (inputBinds[p1Right].contains(keyCode)) action = p1Right;
			else if (inputBinds[p2Jump].contains(keyCode)) action = p2Jump;
			else if (inputBinds[p2Left].contains(keyCode)) action = p2Left;
			else if (inputBinds[p2Right].contains(keyCode)) action = p2Right;
			else continue;

			player1 = action <= p1Right;
			if (action == p1Jump || action == p2Jump) input.m_button = PlayerButton::Jump;
			else if (action == p1Left || action == p2Left) input.m_button = PlayerButton::Left;
			else input.m_button = PlayerButton::Right;

			if (socdFix) {
				double evTime = (double)ev.time.QuadPart / (double)freq.QuadPart;
				flushPendingReleases(evTime); // anything older than the window was a real release

				auto& held = heldKeys[action];
				auto& pending = pendingReleases[action];

				if (value == Press) {
					bool wasDown = !held.empty();
					held.insert(keyCode);
					if (pending.active) { // release + press back to back: the action never really went up
						pending.active = false;
						continue;
					}
					if (wasDown) continue; // another key already holds this action down
				}
				else {
					held.erase(keyCode);
					if (!held.empty()) continue; // another key still holds this action down

					// last key of this action released: hold it back briefly in case a press follows
					input.m_isPush = false;
					input.m_timestamp = evTime;
					input.m_isPlayer2 = !player1;
					pending.active = true;
					pending.cmd = input;
					continue;
				}
			}
			break;
		}
		case TOUCHSCREEN:
			if (scanCode == BTN_TOUCH) {
				input.m_button = PlayerButton::Jump;
			}
			break;
		case CONTROLLER: {
			int keyCode = -1;
			if (ev.type == EV_KEY) {
				keyCode = linuxToCCKey[scanCode];
			} else if (ev.type == EV_ABS) {
				bool continueLoop = false;
				auto analyze4Directions = [&] (int deadzone, enumKeyCodes negative, enumKeyCodes positive) {
					if (ev.value < -deadzone) {
						keyCode = negative;
						if (heldInputs.contains(negative)) {
							continueLoop = true;
						}
						value = Press;
					} else if (ev.value > deadzone) {
						keyCode = positive;
						if (heldInputs.contains(positive)) {
							continueLoop = true;
						}
						value = Press;
					} else {
						value = Release;
						if (heldInputs.contains(negative)) {
							keyCode = negative;
						} else if (heldInputs.contains(positive)) {
							keyCode = positive;
						} else {
							continueLoop = true;
						}
					}
				};

				switch (ev.code) {
				case ABS_X:
					analyze4Directions(XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, CONTROLLER_LTHUMBSTICK_LEFT, CONTROLLER_LTHUMBSTICK_RIGHT);
					break;
				case ABS_Y:
					analyze4Directions(XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, CONTROLLER_LTHUMBSTICK_UP, CONTROLLER_LTHUMBSTICK_DOWN);
					break;
				case ABS_RX:
					analyze4Directions(XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, CONTROLLER_RTHUMBSTICK_LEFT, CONTROLLER_RTHUMBSTICK_RIGHT);
					break;
				case ABS_RY:
					analyze4Directions(XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, CONTROLLER_RTHUMBSTICK_UP, CONTROLLER_RTHUMBSTICK_DOWN);
					break;
				case ABS_HAT0X:
					analyze4Directions(10, CONTROLLER_Left, CONTROLLER_Right);
					break;
				case ABS_HAT0Y:
					analyze4Directions(10, CONTROLLER_Up, CONTROLLER_Down);
					break;
				case ABS_Z:
					keyCode = CONTROLLER_LT;
					if (ev.value > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
						value = Press;
					} else {
						value = Release;
					}
					break;
				case ABS_RZ:
					keyCode = CONTROLLER_RT;
					if (ev.value > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
						value = Press;
					} else {
						value = Release;
					}
					break;
				}
				if (continueLoop) continue;
			}
			if (inputBinds[p1Jump].contains(keyCode)) input.m_button = PlayerButton::Jump;
			else if (inputBinds[p1Left].contains(keyCode)) input.m_button = PlayerButton::Left;
			else if (inputBinds[p1Right].contains(keyCode)) input.m_button = PlayerButton::Right;
			else {
				player1 = false;
				if (inputBinds[p2Jump].contains(keyCode)) input.m_button = PlayerButton::Jump;
				else if (inputBinds[p2Left].contains(keyCode)) input.m_button = PlayerButton::Left;
				else if (inputBinds[p2Right].contains(keyCode)) input.m_button = PlayerButton::Right;
				else continue;
			}
			if (value == Press) {
				if (heldInputs.contains(keyCode)) {
					continue;
				} else {
					heldInputs.emplace(keyCode);
				}
			} else {
				if (!heldInputs.contains(keyCode)) {
					continue;
				} else {
					heldInputs.erase(keyCode);
				}
			}
			break;
		}
		default:
			continue;
		}

		input.m_isPush = value;
		input.m_timestamp = (double)ev.time.QuadPart / (double)freq.QuadPart;
		input.m_isPlayer2 = !player1;

		inputVector.emplace_back(input);
	}

	if (socdFix) flushPendingReleases(getCurrentTimestamp());

	pSharedMem->tail = t;
}

void windowsSetup() {
	QueryPerformanceFrequency(&freq);

	HMODULE ntdll = GetModuleHandle("ntdll.dll");
	typedef void (*wine_get_host_version)(const char **sysname, const char **release);
	wine_get_host_version wghv = (wine_get_host_version)GetProcAddress(ntdll, "wine_get_host_version");
	if (wghv) { // if this function exists, the user is on Wine
		const char* sysname;
		const char* release;
		wghv(&sysname, &release);

		std::string sys = sysname;
		log::info("Wine {}", sys);

		if (sys == "Linux") Mod::get()->setSavedValue<bool>("you-must-be-on-linux-to-change-this", true);
		if (sys == "Linux" && Mod::get()->getSettingValue<bool>("wine-workaround")) { // background raw keyboard input doesn't work in Wine
			linuxNative = true;
			log::info("Linux native");

			std::string shmName = "cbf-" + std::to_string(GetCurrentProcessId());
			std::string winShmPath = std::string("Z:\\dev\\shm\\") + shmName;
			std::string unixShmPath = std::string("/dev/shm/") + shmName;

			hShmFile = CreateFile(winShmPath.c_str(), GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if (hShmFile == INVALID_HANDLE_VALUE) {
				log::error("Failed to create shared memory file: {}", GetLastError());
				return;
			}

			LARGE_INTEGER fileSize;
			fileSize.QuadPart = sizeof(SharedMemory);
			SetFilePointerEx(hShmFile, fileSize, NULL, FILE_BEGIN);
			SetEndOfFile(hShmFile);

			hShmMapping = CreateFileMapping(hShmFile, NULL, PAGE_READWRITE, 0, sizeof(SharedMemory), NULL);
			if (!hShmMapping) {
				log::error("Failed to create file mapping: {}", GetLastError());
				CloseHandle(hShmFile);
				return;
			}

			pSharedMem = static_cast<SharedMemory*>(
				MapViewOfFile(hShmMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemory)));
			if (!pSharedMem) {
				log::error("Failed to map view: {}", GetLastError());
				CloseHandle(hShmMapping);
				CloseHandle(hShmFile);
				return;
			}

			ZeroMemory(pSharedMem, sizeof(SharedMemory));

			std::string path = CCFileUtils::get()->fullPathForFilename("linux-input.so"_spr, true);

			typedef char* (CDECL *wine_get_unix_file_name_t)(LPCWSTR);
			auto wgufn = (wine_get_unix_file_name_t)GetProcAddress(
				GetModuleHandle("kernel32.dll"), "wine_get_unix_file_name");

			std::string unixBinPath;
			if (wgufn) {
				int wlen = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, NULL, 0);
				std::vector<wchar_t> wpath(wlen);
				MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wpath.data(), wlen);
				char* ubp = wgufn(wpath.data());
				if (ubp) {
					unixBinPath = ubp;
					HeapFree(GetProcessHeap(), 0, ubp);
				}
			}

			if (unixBinPath.empty()) {
				log::error("Failed to resolve Unix path for linux-input binary");
				UnmapViewOfFile(pSharedMem);
				CloseHandle(hShmMapping);
				CloseHandle(hShmFile);
				pSharedMem = nullptr;
				return;
			}

			STARTUPINFO si;
			PROCESS_INFORMATION pi;
			ZeroMemory(&si, sizeof(si));
			si.cb = sizeof(si);
			ZeroMemory(&pi, sizeof(pi));

			std::string cmdline = std::string("/bin/sh -c \"chmod +x '") + unixBinPath
				+ "' && exec '" + unixBinPath + "' '" + unixShmPath + "'\"";

			if (!CreateProcess("Z:\\bin\\sh", (LPSTR)cmdline.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
				log::error("Failed to launch Linux input program: {}", GetLastError());
				UnmapViewOfFile(pSharedMem);
				CloseHandle(hShmMapping);
				CloseHandle(hShmFile);
				pSharedMem = nullptr;
				return;
			}

			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
		}
	}
}
