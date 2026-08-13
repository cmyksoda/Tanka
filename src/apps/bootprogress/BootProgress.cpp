/*
 * Copyright 2026, Sean Malseed.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Sean Malseed
 *		Claude (Anthropic), paired via Claude Code
 */


// A tiny boot-progress panel for the Tanka distribution. On the FIRST boot,
// package activation on old PPC hardware is slow, so this shows a "setting up"
// message on top of the bare desktop and quits once activation is done and the
// desktop is up, then drops a marker so it never shows again.
//
// The "still working" signal is the package daemon's first-boot transaction
// directory: it is created at package_daemon init (before this app launches)
// and removed when first-boot processing finishes, so it exists for the whole
// slow stretch - no early-quiet race like watching Deskbar or the team count.


#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Font.h>
#include <MessageRunner.h>
#include <OS.h>
#include <Path.h>
#include <Roster.h>
#include <Screen.h>
#include <String.h>
#include <View.h>
#include <Window.h>

#include <string.h>


static const char* kDeskbarSignature = "application/x-vnd.Be-TSKB";
static const char* kMarkerName = "tanka_first_boot_seen";
static const char* kAdminDirs[] = {
	"/boot/system/packages/administrative",
	"/boot/home/config/packages/administrative",
	NULL
};
static const uint32 kPollWhat = 'poll';
static const bigtime_t kPollInterval = 500000;		// 0.5 s
static const int32 kMinTicks = 12;					// show at least ~6 s
static const int32 kSettleTicks = 20;				// 10 s of no team churn (fallback)
static const int32 kQuitAfterTicks = 960;			// ~8 min safety timeout


static bool
marker_path(BPath& path)
{
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return false;
	return path.Append(kMarkerName) == B_OK;
}


// True while the package daemon still has a first-boot (or any) transaction
// directory open in an administrative folder.
static bool
package_processing_active()
{
	for (int i = 0; kAdminDirs[i] != NULL; i++) {
		BDirectory dir(kAdminDirs[i]);
		if (dir.InitCheck() != B_OK)
			continue;
		BEntry entry;
		char name[B_FILE_NAME_LENGTH];
		while (dir.GetNextEntry(&entry) == B_OK) {
			if (entry.GetName(name) == B_OK
				&& strncmp(name, "transaction-", 12) == 0) {
				return true;
			}
		}
	}
	return false;
}


class ProgressView : public BView {
public:
	ProgressView(BRect frame)
		:
		BView(frame, "progress", B_FOLLOW_ALL, B_WILL_DRAW),
		fDots(0)
	{
		SetViewColor(38, 42, 50);
	}

	void Draw(BRect updateRect)
	{
		SetHighColor(240, 240, 240);
		BFont bold(be_bold_font);
		bold.SetSize(18);
		SetFont(&bold);
		_DrawCentered("Setting up your system", 48);

		BFont plain(be_plain_font);
		plain.SetSize(12);
		SetFont(&plain);
		SetHighColor(198, 200, 208);
		_DrawCentered("First boot can take a few minutes on older hardware.", 80);
		_DrawCentered("Please don't power off.", 100);

		BString wait("Working");
		for (int32 i = 0; i < fDots; i++)
			wait << ".";
		_DrawCentered(wait.String(), 128);
	}

	void Tick()
	{
		fDots = (fDots + 1) % 4;
		Invalidate();
	}

private:
	void _DrawCentered(const char* text, float y)
	{
		float width = StringWidth(text);
		DrawString(text, BPoint((Bounds().Width() - width) / 2.0f, y));
	}

	int32 fDots;
};


class ProgressWindow : public BWindow {
public:
	ProgressWindow(BRect frame)
		:
		BWindow(frame, "Setting up", B_NO_BORDER_WINDOW_LOOK,
			B_FLOATING_ALL_WINDOW_FEEL,
			B_NOT_MOVABLE | B_NOT_CLOSABLE | B_NOT_ZOOMABLE
				| B_NOT_MINIMIZABLE | B_AVOID_FOCUS)
	{
		fView = new ProgressView(Bounds());
		AddChild(fView);
	}

	void Tick()
	{
		if (Lock()) {
			fView->Tick();
			Unlock();
		}
	}

private:
	ProgressView* fView;
};


class BootProgressApp : public BApplication {
public:
	BootProgressApp()
		:
		BApplication("application/x-vnd.Tanka-BootProgress"),
		fWindow(NULL),
		fRunner(NULL),
		fTicks(0),
		fLastTeamCount(-1),
		fSawChurn(false),
		fStableTicks(0)
	{
	}

	void ReadyToRun()
	{
		// Only ever show on the first boot.
		BPath path;
		if (marker_path(path)) {
			BEntry entry(path.Path());
			if (entry.Exists()) {
				PostMessage(B_QUIT_REQUESTED);
				return;
			}
		}

		_ShowWindow();
		fRunner = new BMessageRunner(be_app_messenger,
			new BMessage(kPollWhat), kPollInterval);
	}

	void MessageReceived(BMessage* message)
	{
		if (message->what != kPollWhat) {
			BApplication::MessageReceived(message);
			return;
		}

		fTicks++;

		// Fallback "settled" signal: first-boot activation churns the team count
		// (a post-install script per package); once that stops for a while the
		// system is done, even if the transaction directory happens to linger.
		system_info info;
		if (get_system_info(&info) == B_OK) {
			if ((int32)info.used_teams != fLastTeamCount) {
				fLastTeamCount = (int32)info.used_teams;
				fSawChurn = true;
				fStableTicks = 0;
			} else
				fStableTicks++;
		}

		bool deskbarUp = be_roster->IsRunning(kDeskbarSignature);
		bool busy = package_processing_active();
		bool settled = fSawChurn && fStableTicks >= kSettleTicks;

		// Done when the desktop is up and either package processing finished or
		// the system has clearly settled, after a brief minimum display.
		if ((deskbarUp && fTicks >= kMinTicks && (!busy || settled))
				|| fTicks >= kQuitAfterTicks) {
			if (deskbarUp)
				_WriteMarker();
			PostMessage(B_QUIT_REQUESTED);
			return;
		}

		if (fWindow != NULL)
			fWindow->Tick();
	}

private:
	void _ShowWindow()
	{
		BScreen screen;
		BRect screenFrame = screen.Frame();
		const float width = 420;
		const float height = 165;
		float left = (screenFrame.Width() - width) / 2.0f;
		float top = (screenFrame.Height() - height) / 2.0f;
		fWindow = new ProgressWindow(
			BRect(left, top, left + width, top + height));
		fWindow->Show();
	}

	void _WriteMarker()
	{
		BPath path;
		if (!marker_path(path))
			return;
		BFile file(path.Path(), B_CREATE_FILE | B_ERASE_FILE | B_WRITE_ONLY);
	}

	ProgressWindow*		fWindow;
	BMessageRunner*		fRunner;
	int32				fTicks;
	int32				fLastTeamCount;
	bool				fSawChurn;
	int32				fStableTicks;
};


int
main()
{
	BootProgressApp app;
	app.Run();
	return 0;
}
