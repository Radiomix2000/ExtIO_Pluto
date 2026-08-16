
#define HWNAME				"Pluto"
#define HWMODEL				"Pluto"
#define VERNUM              "0.01"
#define SETTINGS_IDENTIFIER	"Pluto-1.x"
#define LO_MIN				70000000LL
#define LO_MAX				6000000000LL
#define EXT_BLOCKLEN		512 * 4		/* only multiples of 512 */

// Needed so WM_MOUSEWHEEL / GET_WHEEL_DELTA_WPARAM are defined regardless
// of the Windows SDK's default target version.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif

#include "iio.h"
#include "ExtIO_Pluto.h"

//---------------------------------------------------------------------------
#include <windows.h>
#include <windowsx.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "resource.h"

//---------------------------------------------------------------------------

#ifdef _DEBUG
#define _MYDEBUG // Activate a debug console
#endif

#ifdef  _MYDEBUG
/* Debug Trace Enabled */
#define DbgPrintf printf
#else
/* Debug Trace Disabled */
#define DbgPrintf(Message) MessageBoxA(NULL, Message, NULL, MB_OK|MB_ICONERROR) 
#endif

#define MHZ(x) ((long long)(x*1000000.0 + .5))
#define GHZ(x) ((long long)(x*1000000000.0 + .5))

static char gSDR[1025] = "ip:192.168.2.1\0";

/* RX is input, TX is output */
enum iodev { RX, TX };

/* common RX and TX streaming params */
struct stream_cfg {
	long long bw_hz; // Analog banwidth in Hz
	long long fs_hz; // Baseband sample rate in Hz
	long long lo_hz; // Local oscillator frequency in Hz
	const char* rfport; // Port name
};

/* IIO structs required for streaming */
static struct iio_context *ctx = NULL;
static struct iio_channel *rx0_i = NULL;
static struct iio_channel *rx0_q = NULL;
static struct iio_buffer  *rxbuf = NULL;

// Streaming devices
static struct iio_device *rx = NULL;

// Stream configurations
static struct stream_cfg rxcfg;

#pragma warning(disable : 4996)

#define snprintf	_snprintf

static bool SDR_supports_settings = false;  // assume not supported
static bool SDR_settings_valid = false;		// assume settings are for some other ExtIO

static char SDR_progname[32+1] = "\0";
static int  SDR_ver_major = -1;
static int  SDR_ver_minor = -1;

static int		gHwType = exthwUSBdata16; // 16bit UINT
static int		giExtSrateIdx = 4;	// default: index of 2.5 MS/s in the sorted preset list below
static unsigned gExtSampleRate = 2500000; // just default

// --- Dialog window position -------------------------------------------------
// -1 means "not saved yet" - fall back to centering the dialog on screen.
static int gDialogX = -1;
static int gDialogY = -1;

// --- RX gain control ------------------------------------------------------
// AD9361 gain_control_mode values, in the order shown in IDC_COMBO_GAINMODE
static const char* kGainModes[4] = { "manual", "slow_attack", "fast_attack", "hybrid" };
static int		gGainModeIdx = 0;			// 0 = manual
static float	gGainDB = 30.0f;			// manual RX gain, dB

// --- RX filter (analog) bandwidth -----------------------------------------
static long long gBWHz = 2000000LL;		// current analog filter bandwidth, Hz
static bool		gBWAuto = true;				// true: bandwidth follows sample rate (0.8*fs), as before
												// false: bandwidth was set explicitly by the user in GUI

// Selectable mouse-wheel step for the RX filter BW field, in the order
// shown in IDC_COMBO_BWSTEP.
static const long long kBWSteps[5]     = { 1000LL, 10000LL, 100000LL, 1000000LL, 10000000LL };
static const char*     kBWStepLabels[5] = { "1 kHz", "10 kHz", "100 kHz", "1 MHz", "10 MHz" };
static int gBWStepIdx = 1;	// default: 10 kHz

volatile int64_t	glLOfreq = 0L;
bool	gbInitHW = false;

pfnExtIOCallback	pfnCallback = 0;
volatile bool	gbExitThread = false;
volatile bool	gbThreadRunning = false;

///---
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
HWND h_dialog = nullptr;
HANDLE thread_handle = INVALID_HANDLE_VALUE;

//---------------------------------------------------------------------------
// Persist Gain / BW / Sample-rate / URI settings to a plain text file next
// to ExtIO_Pluto.dll itself, independent of whatever HDSDR's own
// ExtIoGetSetting/SetSetting round-trip does (that path turned out
// unreliable in testing), and independent of the Windows Registry (calling
// Advapi32 registry functions from DllMain risks the classic loader-lock
// deadlock/failure, since Advapi32.dll may not yet be loaded at that
// point). Plain file I/O only touches kernel32/CRT, which are always
// already available, so this is the safe, guaranteed path.
static void GetSettingsFilePath(char* outPath, size_t outSize)
{
	char modulePath[MAX_PATH] = { 0 };
	GetModuleFileNameA((HMODULE)&__ImageBase, modulePath, MAX_PATH);

	// Strip the file name, keep just the directory ExtIO_Pluto.dll lives in.
	char* lastSlash = strrchr(modulePath, '\\');
	if (lastSlash) *(lastSlash + 1) = '\0';
	else modulePath[0] = '\0';

	snprintf(outPath, outSize, "%sExtIO_Pluto_settings.ini", modulePath);
}

static void SaveSettingsToFile()
{
	char path[MAX_PATH + 64];
	GetSettingsFilePath(path, sizeof(path));

	FILE* f = fopen(path, "w");
	if (!f) {
		DbgPrintf("SaveSettingsToFile: fopen failed for write\n");
		return;
	}

	fprintf(f, "GainModeIdx=%d\n", gGainModeIdx);
	fprintf(f, "GainDB=%.2f\n", gGainDB);
	fprintf(f, "BWHz=%lld\n", gBWHz);
	fprintf(f, "BWAuto=%d\n", gBWAuto ? 1 : 0);
	fprintf(f, "BWStepIdx=%d\n", gBWStepIdx);
	fprintf(f, "SampleRateIdx=%d\n", giExtSrateIdx);
	fprintf(f, "SDR=%s\n", gSDR);
	fprintf(f, "DialogX=%d\n", gDialogX);
	fprintf(f, "DialogY=%d\n", gDialogY);

	fclose(f);

#ifdef _MYDEBUG
	printf("SaveSettingsToFile: wrote %s (GainDB=%.2f BWHz=%lld BWAuto=%d)\n",
		path, gGainDB, gBWHz, gBWAuto ? 1 : 0);
#endif
}

static void LoadSettingsFromFile()
{
	char path[MAX_PATH + 64];
	GetSettingsFilePath(path, sizeof(path));

	FILE* f = fopen(path, "r");
	if (!f) {
#ifdef _MYDEBUG
		printf("LoadSettingsFromFile: no settings file at %s (using defaults)\n", path);
#endif
		return;	// nothing saved yet - keep the compiled-in defaults
	}

	int linesRead = 0;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		linesRead++;
		char* eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		char* key = line;
		char* val = eq + 1;

		// strip trailing CR/LF from the value
		size_t vlen = strlen(val);
		while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r')) {
			val[--vlen] = '\0';
		}

		if      (strcmp(key, "GainModeIdx")   == 0) { int v = atoi(val); if (v >= 0 && v <= 3) gGainModeIdx = v; }
		else if (strcmp(key, "GainDB")        == 0) { gGainDB = (float)atof(val); }
		else if (strcmp(key, "BWHz")          == 0) { gBWHz = _atoi64(val); }
		else if (strcmp(key, "BWAuto")        == 0) { gBWAuto = (atoi(val) != 0); }
		else if (strcmp(key, "BWStepIdx")     == 0) { int v = atoi(val); if (v >= 0 && v < 5) gBWStepIdx = v; }
		else if (strcmp(key, "SampleRateIdx") == 0) { int v = atoi(val); if (v >= 0 && v < 10) giExtSrateIdx = v; }
		else if (strcmp(key, "SDR")           == 0) { strncpy(gSDR, val, sizeof(gSDR) - 1); gSDR[sizeof(gSDR) - 1] = '\0'; }
		else if (strcmp(key, "DialogX")       == 0) { gDialogX = atoi(val); }
		else if (strcmp(key, "DialogY")       == 0) { gDialogY = atoi(val); }
	}

	fclose(f);

	// giExtSrateIdx alone isn't enough to run the receiver - resolve it to
	// an actual Hz value too, same as ExtIoSetSetting would have done.
	double sr = 0.0;
	if (0 == ExtIoGetSrates(giExtSrateIdx, &sr)) {
		gExtSampleRate = (unsigned)(sr + 0.5);
	}

#ifdef _MYDEBUG
	printf("LoadSettingsFromFile: read %d line(s) from %s -> GainDB=%.2f BWHz=%lld BWAuto=%d GainModeIdx=%d SampleRateIdx=%d DialogX=%d DialogY=%d\n",
		linesRead, path, gGainDB, gBWHz, gBWAuto ? 1 : 0, gGainModeIdx, giExtSrateIdx, gDialogX, gDialogY);
#endif
}

//---------------------------------------------------------------------------
// Returns the RX ("voltage0", input) channel of the ad9361-phy device,
// used for both gain and analog filter bandwidth attributes.
static struct iio_channel* GetRxPhyChan()
{
	if (!ctx) return NULL;
	struct iio_device* phy = iio_context_find_device(ctx, "ad9361-phy");
	if (!phy) return NULL;
	return iio_device_find_channel(phy, "voltage0", false);
}

//---------------------------------------------------------------------------
// Pushes the current gain mode / manual gain value to the hardware.
static void ApplyGain()
{
	struct iio_channel* chn = GetRxPhyChan();
	if (!chn) return;

	if (iio_channel_attr_write(chn, "gain_control_mode", kGainModes[gGainModeIdx]) < 0) {
		DbgPrintf("gain_control_mode set failed\n");
	}
	if (gGainModeIdx == 0) { // manual gain only makes sense in manual mode
		if (iio_channel_attr_write_double(chn, "hardwaregain", (double)gGainDB) < 0) {
			DbgPrintf("hardwaregain set failed\n");
		}
	}
}

//---------------------------------------------------------------------------
// Pushes the current analog RX filter bandwidth to the hardware.
static void ApplyBandwidth()
{
	struct iio_channel* chn = GetRxPhyChan();
	if (!chn) return;

	if (iio_channel_attr_write_longlong(chn, "rf_bandwidth", gBWHz) < 0) {
		DbgPrintf("rf_bandwidth set failed\n");
	}
}

//---------------------------------------------------------------------------
// Common implementation for sample-rate change, used both by the host
// (ExtIoSetSrate, called by HDSDR) and by the GUI's own sample-rate combo box.
static int ApplySampleRateIdx(int srate_idx)
{
	double newSrate = 0.0;
	if (0 != ExtIoGetSrates(srate_idx, &newSrate))
		return 1;	// ERROR

	giExtSrateIdx = srate_idx;
	gExtSampleRate = (unsigned)(newSrate + 0.5);
	rxcfg.fs_hz = gExtSampleRate;

	if (gBWAuto) {
		gBWHz = (long long)(gExtSampleRate * 0.8);
	}
	rxcfg.bw_hz = gBWHz;

	if (ctx) {
		struct iio_channel* chn = GetRxPhyChan();
		if (chn == NULL) {
			DbgPrintf("chn not created\n");
			return 1;
		};
		if (iio_channel_attr_write_longlong(chn, "rf_bandwidth", rxcfg.bw_hz) < 0) {
			DbgPrintf("rf_bandwidth set failed\n");
			return 1;
		};
		if (iio_channel_attr_write_longlong(chn, "sampling_frequency", rxcfg.fs_hz) < 0) {
			DbgPrintf("sampling_frequency set failed\n");
			return 1;
		};
	}
	else return 1;

	return 0;
}

//---------------------------------------------------------------------------
void UpdateDialog()
{
	if (!h_dialog) return;

	char buf[32];

	SetDlgItemTextA(h_dialog, IDC_ALPF_BW, gSDR);

	snprintf(buf, sizeof(buf), "%.1f", gGainDB);
	SetDlgItemTextA(h_dialog, IDC_EDIT_GAIN, buf);

	snprintf(buf, sizeof(buf), "%lld", gBWHz);
	SetDlgItemTextA(h_dialog, IDC_EDIT_BW, buf);

	SendDlgItemMessageA(h_dialog, IDC_COMBO_GAINMODE, CB_SETCURSEL, (WPARAM)gGainModeIdx, 0);
	EnableWindow(GetDlgItem(h_dialog, IDC_EDIT_GAIN), gGainModeIdx == 0);

	SendDlgItemMessageA(h_dialog, IDC_COMBO_SRATE, CB_SETCURSEL, (WPARAM)giExtSrateIdx, 0);

	SendDlgItemMessageA(h_dialog, IDC_COMBO_BWSTEP, CB_SETCURSEL, (WPARAM)gBWStepIdx, 0);
}

//---------------------------------------------------------------------------
// Mouse-wheel adjustment for the RX Gain / RX filter BW edit boxes: click
// into the field to give it focus (Windows routes WM_MOUSEWHEEL to the
// focused control), then each wheel notch changes the value by one step
// (1 dB for gain, 100 Hz for bandwidth) and applies it immediately.
static WNDPROC gOrigEditGainProc = NULL;
static WNDPROC gOrigEditBWProc   = NULL;

// Guards the Gain/BW EN_CHANGE ("apply as you type") handlers against
// phantom notifications: creating IDC_EDIT_GAIN/IDC_EDIT_BW from the .rc
// dialog template (which gives them placeholder text "0" / "2000000")
// itself sends WM_SETTEXT, and Windows fires EN_CHANGE for that too - this
// happens BEFORE our WM_INITDIALOG/UpdateDialog() gets to show the real,
// just-restored values. Without this guard, that phantom EN_CHANGE reads
// the placeholder text and immediately overwrites the freshly-loaded
// settings (both in memory and in the settings file) with "0"/"2000000".
// Only set true once UpdateDialog() has run at the end of WM_INITDIALOG.
static bool gDialogReady = false;

static LRESULT CALLBACK EditGainSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_MOUSEWHEEL) {
		int steps = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
		if (steps != 0) {
			char buf[32];
			GetWindowTextA(hwnd, buf, sizeof(buf));
			float g = (float)atof(buf) + steps * 1.0f; // 1 dB per wheel notch
			if (g < -10.0f) g = -10.0f;
			if (g > 77.0f)  g = 77.0f;
			gGainDB = g;
			snprintf(buf, sizeof(buf), "%.1f", gGainDB);
			SetWindowTextA(hwnd, buf);
			ApplyGain();
			SaveSettingsToFile();
		}
		return 0;
	}
	return CallWindowProc(gOrigEditGainProc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK EditBWSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_MOUSEWHEEL) {
		int steps = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
		if (steps != 0) {
			char buf[32];
			GetWindowTextA(hwnd, buf, sizeof(buf));
			long long bw = _atoi64(buf) + (long long)steps * kBWSteps[gBWStepIdx]; // step from IDC_COMBO_BWSTEP
			if (bw < 200000LL)   bw = 200000LL;
			if (bw > 56000000LL) bw = 56000000LL;
			gBWHz = bw;
			gBWAuto = false;	// user took manual control of the filter bandwidth
			rxcfg.bw_hz = gBWHz;
			snprintf(buf, sizeof(buf), "%lld", gBWHz);
			SetWindowTextA(hwnd, buf);
			ApplyBandwidth();
			SaveSettingsToFile();
		}
		return 0;
	}
	return CallWindowProc(gOrigEditBWProc, hwnd, msg, wParam, lParam);
}

//---------------------------------------------------------------------------
static INT_PTR CALLBACK MainDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{

	switch (uMsg) {

	case WM_INITDIALOG:
	{
		gDialogReady = false;	// suppress phantom EN_CHANGE while controls are (re)built below

		// Position the dialog: restore the last saved position, or center
		// it on the (primary monitor's) screen the first time it's ever
		// shown (it otherwise always appears pinned at (0,0), top-left).
		{
			RECT rc;
			GetWindowRect(hwndDlg, &rc);
			int dlgWidth  = rc.right  - rc.left;
			int dlgHeight = rc.bottom - rc.top;

			RECT work;
			SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);	// excludes the taskbar
			int workWidth  = work.right  - work.left;
			int workHeight = work.bottom - work.top;

			int x, y;
			if (gDialogX >= 0 && gDialogY >= 0) {
				// Restore saved position, clamped onto the visible work area
				// in case the screen resolution changed since it was saved.
				x = gDialogX;
				y = gDialogY;
				if (x + dlgWidth  > work.right)  x = work.right  - dlgWidth;
				if (y + dlgHeight > work.bottom) y = work.bottom - dlgHeight;
				if (x < work.left) x = work.left;
				if (y < work.top)  y = work.top;
			}
			else {
				// No saved position yet - center it.
				x = work.left + (workWidth  - dlgWidth)  / 2;
				y = work.top  + (workHeight - dlgHeight) / 2;
			}

			// HWND_TOPMOST here (instead of NULL + SWP_NOZORDER) also makes
			// the dialog stay above HDSDR's main window at all times, even
			// when it doesn't have focus - see the note below.
			SetWindowPos(hwndDlg, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE);
		}

		// Populate sample-rate combo box from the same list HDSDR itself uses
		HWND hComboSR = GetDlgItem(hwndDlg, IDC_COMBO_SRATE);
		for (int i = 0; ; i++) {
			double sr;
			if (0 != ExtIoGetSrates(i, &sr)) break;
			char item[32];
			snprintf(item, sizeof(item), "%.3f MS/s", sr / 1.0e6);
			SendMessageA(hComboSR, CB_ADDSTRING, 0, (LPARAM)item);
		}

		// Populate gain-mode combo box
		HWND hComboGM = GetDlgItem(hwndDlg, IDC_COMBO_GAINMODE);
		SendMessageA(hComboGM, CB_ADDSTRING, 0, (LPARAM)"Manual");
		SendMessageA(hComboGM, CB_ADDSTRING, 0, (LPARAM)"Slow AGC");
		SendMessageA(hComboGM, CB_ADDSTRING, 0, (LPARAM)"Fast AGC");
		SendMessageA(hComboGM, CB_ADDSTRING, 0, (LPARAM)"Hybrid AGC");

		// Subclass the Gain / BW edit boxes so the mouse wheel can adjust
		// their value while the caret is in the field.
		gOrigEditGainProc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hwndDlg, IDC_EDIT_GAIN),
			GWLP_WNDPROC, (LONG_PTR)EditGainSubclassProc);
		gOrigEditBWProc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hwndDlg, IDC_EDIT_BW),
			GWLP_WNDPROC, (LONG_PTR)EditBWSubclassProc);

		// Populate BW mouse-wheel step combo box
		HWND hComboBWStep = GetDlgItem(hwndDlg, IDC_COMBO_BWSTEP);
		for (int i = 0; i < 5; i++) {
			SendMessageA(hComboBWStep, CB_ADDSTRING, 0, (LPARAM)kBWStepLabels[i]);
		}

		UpdateDialog();		// shows the real, just-restored values
		gDialogReady = true;	// EN_CHANGE from here on reflects genuine user input
		return TRUE;
	}
	break;

	case WM_COMMAND:
	{
		switch (GET_WM_COMMAND_ID(wParam, lParam)) {
		case IDC_BUTTON_CAl:
		{
			if (GET_WM_COMMAND_CMD(wParam, lParam) == BN_CLICKED) {

				if (gbThreadRunning)
				{
					MessageBoxA(NULL, "Stop receiver first!", "Error", MB_OK | MB_ICONEXCLAMATION);
					return TRUE;
				}

				int buffSize = GetWindowTextLength(GetDlgItem(hwndDlg, IDC_ALPF_BW));
				char *textBuffer = new char[buffSize + 1];

				GetDlgItemTextA(hwndDlg, IDC_ALPF_BW, textBuffer, buffSize + 1);

				strcpy(gSDR, textBuffer);
				free(textBuffer);

				UpdateDialog();

				if (ctx) { iio_context_destroy(ctx); };
				ctx = iio_create_context_from_uri(gSDR);
				if (ctx == NULL) {
					gbInitHW = false;
					MessageBoxA(NULL, "Connection failed!", "Error", MB_OK | MB_ICONERROR);
				}
				else
				{
					gbInitHW = true;
					// re-apply gain / bandwidth to the freshly (re)created context
					ApplyGain();
					ApplyBandwidth();
					SaveSettingsToFile();
					MessageBoxA(NULL, "Connection successful!", "Info", MB_OK | MB_ICONINFORMATION);
				};

				return TRUE;
			}
		}
		break;

		case IDC_COMBO_SRATE:
		{
			int cmd = GET_WM_COMMAND_CMD(wParam, lParam);

			if (cmd == CBN_SELCHANGE) {
				// User picked one of the fixed presets from the dropdown list.
				int idx = (int)SendDlgItemMessageA(hwndDlg, IDC_COMBO_SRATE, CB_GETCURSEL, 0, 0);
				if (idx >= 0) {
					ApplySampleRateIdx(idx);
					UpdateDialog();
					if (pfnCallback) pfnCallback(-1, extHw_Changed_SampleRate, 0.0F, 0);
					SaveSettingsToFile();
				}
				return TRUE;
			}
		}
		break;

		case IDC_COMBO_GAINMODE:
		{
			if (GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE) {
				int idx = (int)SendDlgItemMessageA(hwndDlg, IDC_COMBO_GAINMODE, CB_GETCURSEL, 0, 0);
				if (idx >= 0) {
					gGainModeIdx = idx;
					ApplyGain();
					UpdateDialog();
					SaveSettingsToFile();
				}
				return TRUE;
			}
		}
		break;

		case IDC_COMBO_BWSTEP:
		{
			if (GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE) {
				int idx = (int)SendDlgItemMessageA(hwndDlg, IDC_COMBO_BWSTEP, CB_GETCURSEL, 0, 0);
				if (idx >= 0 && idx < 5) {
					gBWStepIdx = idx;	// only changes the mouse-wheel step; does not touch gBWHz itself
					SaveSettingsToFile();
				}
				return TRUE;
			}
		}
		break;

		case IDC_EDIT_GAIN:
		{
			// Apply on the fly as the user types, without a separate Apply button.
			if (GET_WM_COMMAND_CMD(wParam, lParam) == EN_CHANGE) {
				if (!gDialogReady) return TRUE;	// ignore phantom EN_CHANGE from control creation

				static bool reentrant = false;
				if (reentrant) return TRUE;
				reentrant = true;

				char buf[32];
				GetDlgItemTextA(hwndDlg, IDC_EDIT_GAIN, buf, sizeof(buf));
				float g = (float)atof(buf);
				bool clamped = false;
				// AD9361 manual RX gain is roughly -3..71 dB depending on band; clamp to a safe range
				if (g < -10.0f) { g = -10.0f; clamped = true; }
				if (g > 77.0f)  { g = 77.0f;  clamped = true; }
				gGainDB = g;
				ApplyGain();
				if (clamped) {
					char buf2[32];
					snprintf(buf2, sizeof(buf2), "%.1f", gGainDB);
					SetDlgItemTextA(hwndDlg, IDC_EDIT_GAIN, buf2);
				}
				SaveSettingsToFile();

				reentrant = false;
				return TRUE;
			}
		}
		break;

		case IDC_EDIT_BW:
		{
			// Apply on the fly as the user types, without a separate Apply button.
			if (GET_WM_COMMAND_CMD(wParam, lParam) == EN_CHANGE) {
				if (!gDialogReady) return TRUE;	// ignore phantom EN_CHANGE from control creation

				static bool reentrant = false;
				if (reentrant) return TRUE;
				reentrant = true;

				char buf[32];
				GetDlgItemTextA(hwndDlg, IDC_EDIT_BW, buf, sizeof(buf));
				long long bw = _atoi64(buf);
				bool clamped = false;
				// AD9361 analog RX filter bandwidth range is roughly 200 kHz .. 56 MHz
				if (bw < 200000LL)   { bw = 200000LL;   clamped = true; }
				if (bw > 56000000LL) { bw = 56000000LL; clamped = true; }
				gBWHz = bw;
				gBWAuto = false;	// user took manual control of the filter bandwidth
				rxcfg.bw_hz = gBWHz;
				ApplyBandwidth();
				if (clamped) {
					char buf2[32];
					snprintf(buf2, sizeof(buf2), "%lld", gBWHz);
					SetDlgItemTextA(hwndDlg, IDC_EDIT_BW, buf2);
				}
				SaveSettingsToFile();

				reentrant = false;
				return TRUE;
			}
		}
		break;
		}
	}
	break;

	case WM_SHOWWINDOW:
		UpdateDialog();
		return TRUE;
		break;

	case WM_MOVE:
	{
		// Track the current position in memory as the user drags the
		// window; the actual file write is deferred to WM_EXITSIZEMOVE
		// (drag finished) to avoid hammering the disk while dragging.
		RECT rc;
		GetWindowRect(hwndDlg, &rc);
		gDialogX = rc.left;
		gDialogY = rc.top;
	}
	break;

	case WM_EXITSIZEMOVE:
		SaveSettingsToFile();
		break;

	case WM_CLOSE:
		ShowWindow(h_dialog, SW_HIDE);
		return TRUE;
		break;

	case WM_DESTROY:
		ShowWindow(h_dialog, SW_HIDE);
		h_dialog = NULL;
		return TRUE;
		break;
	}
	return FALSE;
}

//---------------------------------------------------------------------------
DWORD WINAPI GeneratorThreadProc( __in  LPVOID lpParameter )
{
	int16_t	iqbuf[EXT_BLOCKLEN * 2];
	ssize_t	nbytes_rx;
	char	*p_dat, *p_end;
	ptrdiff_t	p_inc;

	int	iqcnt = 0; // pointer to sample in iqbuf
	
	while ( !gbExitThread )
	{
		nbytes_rx = iio_buffer_refill(rxbuf);
		//if (nbytes_rx < 0) { printf("Error refilling buf %d\n", (int)nbytes_rx); }
		p_inc = iio_buffer_step(rxbuf);
		p_end = (char *)iio_buffer_end(rxbuf);
		for (p_dat = (char *)iio_buffer_first(rxbuf, rx0_i); p_dat < p_end; p_dat += p_inc) {

			iqbuf[iqcnt++] = ((int16_t*)p_dat)[0];
			iqbuf[iqcnt++] = ((int16_t*)p_dat)[1];

			if (iqcnt == EXT_BLOCKLEN * 2) { // buffer full
				iqcnt = 0;
				pfnCallback(EXT_BLOCKLEN, 0, 0.0F, &iqbuf[0]);
			}
		}
	}
	gbExitThread = false;
	gbThreadRunning = false;
	return 0;
}

static void stopThread()
{
	if ( gbThreadRunning )
	{
		gbExitThread = true;
		while ( gbThreadRunning )
		{
			SleepEx( 10, FALSE );
		}
	}
}

static void startThread()
{
	gbExitThread = false;
	gbThreadRunning = true;

	thread_handle = CreateThread( NULL	// LPSECURITY_ATTRIBUTES lpThreadAttributes
		, (SIZE_T)(64 * 1024)	// SIZE_T dwStackSize
		, GeneratorThreadProc	// LPTHREAD_START_ROUTINE lpStartAddress
		, NULL					// LPVOID lpParameter
		, 0						// DWORD dwCreationFlags
		, NULL					// LPDWORD lpThreadId
		);
	SetThreadPriority(thread_handle, THREAD_PRIORITY_TIME_CRITICAL);
}

//---------------------------------------------------------------------------
BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved )
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

//---------------------------------------------------------------------------
extern "C"
bool __declspec(dllexport) __stdcall InitHW(char *name, char *model, int& type)
{
	/* Create debug console window */
#ifdef _MYDEBUG
	if (AllocConsole()) {
		FILE* f;
		freopen_s(&f, "CONOUT$", "wt", stdout);
		SetConsoleTitle(TEXT("Debug Console ExtIO_Pluto " VERNUM));
		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_RED);
	}
#endif

	// Restore Gain/BW/SampleRate/URI here (not from DllMain - the debug
	// console above doesn't exist yet that early, so any trace print would
	// be silently lost; DllMain is also best kept minimal in general).
	// This runs before we connect using gSDR, and before HDSDR's own
	// (now-inert) ExtIoSetSetting calls, so nothing can clobber it. Guarded
	// by !gbInitHW (like the connection below) so a second InitHW call
	// later in the same session can't stomp on live user edits.
	type = gHwType;
	strcpy(name,  HWNAME);
	strcpy(model, HWMODEL);

	if ( !gbInitHW )
	{
		LoadSettingsFromFile();

		ctx = iio_create_context_from_uri(gSDR);
		if (ctx == NULL) {
			MessageBoxA(NULL, "Connection failed!", "Error", MB_OK | MB_ICONERROR);
		};

		glLOfreq = LO_MIN;	// just a default value
		gbInitHW = true;
	}
	return true;
}

//---------------------------------------------------------------------------
extern "C"
bool EXTIO_API OpenHW(void)
{
	h_dialog = CreateDialog((HINSTANCE)&__ImageBase, MAKEINTRESOURCE(ExtioDialog), NULL, (DLGPROC)MainDlgProc);
	ShowWindow(h_dialog, SW_HIDE);
	return true;
}

//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API StartHW(long LOfreq)
{
	int64_t ret = StartHW64( (int64_t)LOfreq );
	return (int)ret;
}

//---------------------------------------------------------------------------
extern "C"
int64_t EXTIO_API StartHW64(int64_t LOfreq)
{
	struct iio_channel *chn = NULL;

	//DbgPrintf("StartHW64\n");
	if (!gbInitHW)
		return 0;

	stopThread();

	// AD IIO init

	// RX stream config
	if (gBWAuto) { gBWHz = (long long)(gExtSampleRate * 0.8); } // default: 80% of sample rate, as before
	rxcfg.bw_hz = gBWHz;			// analog RX filter bandwidth (Hz) - from GUI, or auto
	rxcfg.fs_hz = gExtSampleRate;	// baseband sample rate (Hz) - from GUI / host
	rxcfg.lo_hz = LOfreq; // MHZ(145.5); // 2.5 GHz rf frequency
	rxcfg.rfport = "A_BALANCED"; // port A (select for rf freq.)

#ifdef _MYDEBUG
	printf("SDR addr: %s\n", gSDR);
	printf("* Acquiring IIO context\n");
#endif
	if (ctx) { iio_context_destroy(ctx); };
	ctx = iio_create_context_from_uri(gSDR);
	if (ctx == NULL) {
		MessageBoxA(NULL, "Connection failed!", "Error", MB_OK | MB_ICONERROR);
		return 0;
	};
#ifdef _MYDEBUG
	if (ctx) {
		printf("Devices count %u \n", iio_context_get_devices_count(ctx));
	};
#endif
	rx = iio_context_find_device(ctx, "cf-ad9361-lpc");
	if (rx == NULL) {
		DbgPrintf("rx not created\n");
		return 0;
	};
#ifdef _MYDEBUG
	if (rx) {
		printf("RX device found\n");
	};
#endif
	// setting reciever
	chn = iio_device_find_channel(iio_context_find_device(ctx, "ad9361-phy"), "voltage0", false);
	if (chn == NULL) {
		DbgPrintf("chn not created\n");
		return 0;
	};
#ifdef _MYDEBUG
	if (chn) {
		printf("chn device found\n");
	};
#endif
	if (iio_channel_attr_write(chn, "rf_port_select", rxcfg.rfport) < 0) {
		DbgPrintf("rf_port_select failed\n");
	};
	if (iio_channel_attr_write_longlong(chn, "rf_bandwidth", rxcfg.bw_hz) < 0) {
		DbgPrintf("rf_bandwidth failed\n");
	};
	if (iio_channel_attr_write_longlong(chn, "sampling_frequency", rxcfg.fs_hz) < 0) {
		DbgPrintf("sampling_frequency failed\n");
	};

	// setting RX gain (mode + manual gain value, from GUI)
	ApplyGain();

	// setting LO
	chn = iio_device_find_channel(iio_context_find_device(ctx, "ad9361-phy"), "altvoltage0", true);
	if (chn == NULL) {
		DbgPrintf("chnLO not created\n");
		return 0;
	};
#ifdef _MYDEBUG
	if (chn) {
		printf("chnLO device found\n");
	};
#endif
	if (iio_channel_attr_write_longlong(chn, "frequency", rxcfg.lo_hz) < 0) {
		DbgPrintf("frequency set failed\n");
	};

	// Initializing AD9361 IIO streaming channels
	rx0_i = iio_device_find_channel(rx, "voltage0", false);
	if (rx0_i == NULL) {
		DbgPrintf("rx0_i not created\n");
		return 0;
	};
#ifdef _MYDEBUG
	if (rx0_i) {
		printf("rx0_i device found\n");
	};
#endif
	rx0_q = iio_device_find_channel(rx, "voltage1", false);
	if (rx0_q == NULL) {
		DbgPrintf("rx0_q not created\n");
		return 0;
	};
#ifdef _MYDEBUG
	if (rx0_q) {
		printf("rx0_q device found\n");
	};
#endif
	// Enabling IIO streaming channels
	iio_channel_enable(rx0_i);
	iio_channel_enable(rx0_q);

	rxbuf = iio_device_create_buffer(rx, 1024 * 16, false);
	if (!rxbuf) {
		DbgPrintf("Could not create RX buffer");
		if (rx0_i) { iio_channel_disable(rx0_i); }
		if (rx0_q) { iio_channel_disable(rx0_q); }
		return 0;
	}
#ifdef _MYDEBUG
	else {
		printf("rxbuf created\n");
	}
#endif
	startThread();

	// number of complex elements returned each
	// invocation of the callback routine
	return EXT_BLOCKLEN;
}

//---------------------------------------------------------------------------
extern "C"
void EXTIO_API StopHW(void)
{
	//DbgPrintf("StopHW\n");

	stopThread();

	if (rxbuf) { iio_buffer_destroy(rxbuf); }
	if (rx0_i) { iio_channel_disable(rx0_i); }
	if (rx0_q) { iio_channel_disable(rx0_q); }

	return;  // nothing to do with this specific HW
}

//---------------------------------------------------------------------------
extern "C"
void EXTIO_API CloseHW(void)
{
//	DbgPrintf("CloseHW\n");
	SaveSettingsToFile();	// final safety-net flush of Gain/BW/SampleRate/URI

	if (gbInitHW )
	{
		if (ctx) { iio_context_destroy(ctx); }
	}
	DestroyWindow(h_dialog);

	gbInitHW = false;
}

//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API SetHWLO(long LOfreq)
{
	int64_t ret = SetHWLO64( (int64_t)LOfreq );
	return (ret & 0xFFFFFFFF);
}

extern "C"
int64_t EXTIO_API SetHWLO64(int64_t LOfreq)
{
//	DbgPrintf("SetHWLO64\n");
	const int64_t wishedLO = LOfreq;
	int64_t ret = 0;

	// check limits
	if ( LOfreq < LO_MIN )
	{
		LOfreq = LO_MIN;
		ret = -LO_MIN;
	}
	else if ( LOfreq > LO_MAX )
	{
		LOfreq = LO_MAX;
		ret = LO_MAX;
	}

	// take frequency
	glLOfreq = LOfreq;

	if ( gbInitHW && ctx )
	{
		struct iio_channel *chn = NULL;

		// setting LO
		chn = iio_device_find_channel(iio_context_find_device(ctx, "ad9361-phy"), "altvoltage0", true);
		if (chn == NULL) {
			DbgPrintf("chnLO not created\n");
			return 0;
		};
#ifdef _MYDEBUG
		if (chn) {
			printf("chnlo device found\n");
		};
#endif
		if (iio_channel_attr_write_longlong(chn, "frequency", glLOfreq) < 0) {
			DbgPrintf("frequency set failed\n");
		};
	}

	if ( wishedLO != LOfreq  &&  pfnCallback )
		pfnCallback( -1, extHw_Changed_LO, 0.0F, 0 );

	// 0 The function did complete without errors.
	// < 0 (a negative number N)
	//     The specified frequency  is  lower than the minimum that the hardware  is capable to generate.
	//     The absolute value of N indicates what is the minimum supported by the HW.
	// > 0 (a positive number N) The specified frequency is greater than the maximum that the hardware
	//     is capable to generate.
	//     The value of N indicates what is the maximum supported by the HW.
	return ret;
}

//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API GetStatus(void)
{
	return 0;  
}

//---------------------------------------------------------------------------
extern "C"
void EXTIO_API SetCallback( pfnExtIOCallback funcptr )
{
	pfnCallback = funcptr;
	return;
}

//---------------------------------------------------------------------------
extern "C"
long EXTIO_API GetHWLO(void)
{
	int64_t	glLOfreq1 = GetHWLO64();
	return (long)( glLOfreq1 & 0xFFFFFFFF );
}

extern "C"
int64_t EXTIO_API GetHWLO64(void)
{
	if (gbInitHW && ctx)
	{
		struct iio_channel *chn = NULL;
		long long frq = 0;

		// setting LO
		chn = iio_device_find_channel(iio_context_find_device(ctx, "ad9361-phy"), "altvoltage0", true);
		if (chn == NULL) {
			DbgPrintf("chnLO not created\n");
			return 0;
		};
#ifdef _MYDEBUG
		if (chn) {
			printf("chnlo device found\n");
		};
#endif
		if (iio_channel_attr_read_longlong(chn, "frequency", &frq) < 0) {
			DbgPrintf("frequency read failed\n");
		};
		glLOfreq = frq;
	}
	return glLOfreq;
}

//---------------------------------------------------------------------------
extern "C"
long EXTIO_API GetHWSR(void)
{
	//DbgPrintf("GetHWSR\n");
	// This DLL controls just an oscillator, not a digitizer
	return gExtSampleRate;
}

//---------------------------------------------------------------------------
extern "C"
void EXTIO_API VersionInfo(const char * progname, int ver_major, int ver_minor)
{
  SDR_progname[0] = 0;
  SDR_ver_major = -1;
  SDR_ver_minor = -1;

  if ( progname )
  {
    strncpy( SDR_progname, progname, sizeof(SDR_progname) -1 );
    SDR_ver_major = ver_major;
    SDR_ver_minor = ver_minor;
  }
}

//---------------------------------------------------------------------------
extern "C"
int EXTIO_API ExtIoGetSrates( int srate_idx, double * samplerate )
{
	// Sorted ascending: lower index = lower rate.
	static const double kPresetSrates[] = {
		528000.0,	// 0
		1000000.0,	// 1
		1536000.0,	// 2
		2048000.0,	// 3
		2500000.0,	// 4
		3000000.0,	// 5
		4000000.0,	// 6
		6000000.0,	// 7
		10000000.0,	// 8
		20000000.0,	// 9
	};
	const int kNumPresets = sizeof(kPresetSrates) / sizeof(kPresetSrates[0]);

	if ( srate_idx < 0 || srate_idx >= kNumPresets )
		return 1;	// ERROR

	*samplerate = kPresetSrates[srate_idx];
	return 0;
}

extern "C"
int  EXTIO_API ExtIoGetActualSrateIdx(void)
{
	return giExtSrateIdx;
}

extern "C"
int  EXTIO_API ExtIoSetSrate( int srate_idx )
{
	// Called by HDSDR when the sample rate is changed from its own UI;
	// shares the implementation with the GUI's own sample-rate combo box.
	int ret = ApplySampleRateIdx(srate_idx);
	UpdateDialog();
	return ret;
}

extern "C"
long EXTIO_API ExtIoGetBandwidth( int srate_idx )
{
	double newSrate = 0.0;
	if ( 0 != ExtIoGetSrates( srate_idx, &newSrate ) )
		return -1L;	// ERROR

	long ret = (long)(newSrate * 0.8);
	return ( ret >= newSrate || ret <= 0L ) ? -1L : ret;
}

//---------------------------------------------------------------------------

extern "C"
int  EXTIO_API ExtIoGetSetting( int idx, char * description, char * value )
{
	switch ( idx )
	{
	case 0: snprintf( description, 1024, "%s", "Identifier" );		snprintf( value, 1024, "%s", SETTINGS_IDENTIFIER );	return 0;
	case 1:	snprintf( description, 1024, "%s", "SampleRateIdx" );	snprintf( value, 1024, "%d", giExtSrateIdx );		return 0;
	case 2:	snprintf( description, 1024, "%s", "SDR");		        snprintf( value, 1024, "%s", gSDR);	return 0;
	case 3:	snprintf( description, 1024, "%s", "GainModeIdx");		snprintf( value, 1024, "%d", gGainModeIdx);	return 0;
	case 4:	snprintf( description, 1024, "%s", "GainDB");			snprintf( value, 1024, "%.1f", gGainDB);	return 0;
	case 5:	snprintf( description, 1024, "%s", "BWHz");			snprintf( value, 1024, "%lld", gBWHz);	return 0;
	case 6:	snprintf( description, 1024, "%s", "BWAuto");			snprintf( value, 1024, "%d", gBWAuto ? 1 : 0);	return 0;
	case 7:	snprintf( description, 1024, "%s", "BWStepIdx");		snprintf( value, 1024, "%d", gBWStepIdx);	return 0;
	default:	return -1;	// ERROR
	}
	return -1;	// ERROR
}

extern "C"
void EXTIO_API ExtIoSetSetting( int idx, const char * value )
{
	// Deliberately a no-op for all our own fields (Gain/BW/SampleRate/URI).
	//
	// HDSDR calls this BEFORE InitHW(), i.e. AFTER our own
	// LoadSettingsFromFile() has already restored gGainDB/gBWHz/etc. from
	// ExtIO_Pluto_settings.ini. If HDSDR's own persisted config doesn't
	// (yet) have entries for these indices, it calls this with empty/zero
	// values - which would silently clobber what we just correctly
	// restored. Since our .ini file is now the single source of truth for
	// these settings, we simply ignore whatever the host passes in here.
	//
	// idx 0 (the identifier handshake) is still acknowledged, purely so
	// SDR_settings_valid/SDR_supports_settings stay consistent for hosts
	// that check them.
	SDR_supports_settings = true;
	if ( idx == 0 )
	{
		SDR_settings_valid = ( value && !strcmp( value, SETTINGS_IDENTIFIER ) );
	}
}

//---------------------------------------------------------------------------
extern "C"
void EXTIO_API ShowGUI()
{
	ShowWindow(h_dialog, SW_SHOW);
	return;
}
//---------------------------------------------------------------------------
extern "C"
void EXTIO_API HideGUI()
{
	ShowWindow(h_dialog, SW_HIDE);
	return;
}
//---------------------------------------------------------------------------
