// Test harness: hosts WeatherView instances in an ordinary window, so the
// glyphs, the menu and the location window can be checked with a screenshot
// without touching Deskbar. Not part of the product.
//
//   g++ -Wno-multichar -o testwin tools/testwin.cpp -lbe -lbnetapi -lnetwork
//   ./testwin glyphs | menu | location [query [pick]]
#define private public
#define main rweather_main
#include "../rweather.cpp"
#undef main
#undef private

static const int kCodes[] = { 0, 0, 2, 2, 3, 45, 61, 80, 71, 95, 51 };
static const bool kDay[] = { true, false, true, false, true, true, true,
	true, true, true, true };

class TestApp : public BApplication {
public:
	TestApp(const char* mode, const char* query, bool pick)
		: BApplication("application/x-vnd.rainygirl-RWeather-test"),
		fMode(mode),
		fQuery(query),
		fPick(pick)
	{
	}

	virtual void ReadyToRun()
	{
		BWindow* window = new BWindow(BRect(200, 200, 760, 240), "rweather test",
			B_TITLED_WINDOW, B_NOT_RESIZABLE | B_QUIT_ON_WINDOW_CLOSE);
		BView* back = new BView(window->Bounds(), "back", B_FOLLOW_ALL, 0);
		back->SetViewColor(ui_color(B_MENU_BACKGROUND_COLOR));
		window->AddChild(back);
		int count = strcmp(fMode, "glyphs") == 0 ? 11 : 1;
		for (int i = 0; i < count; i++) {
			WeatherView* view = new WeatherView(BRect(4 + i * 50, 12,
				4 + i * 50 + 44, 27));
			back->AddChild(view);
			fView = view;
		}
		window->Show();
		if (count > 1) {
			// Let the real fetch land first, then overwrite what it drew.
			snooze(8000000);
			if (window->Lock()) {
				for (int i = 0; i < count; i++) {
					WeatherView* view = (WeatherView*)back->ChildAt(i);
					view->fValid = true;
					view->fWeather.code = kCodes[i];
					view->fWeather.isDay = kDay[i];
					view->fWeather.temperature = -3 + i * 3;
					view->Invalidate();
				}
				window->Unlock();
			}
		}
		if (strcmp(fMode, "glyphs") != 0) {
			snooze(4000000);
			if (window->Lock()) {
				if (strcmp(fMode, "menu") == 0)
					fView->MouseDown(BPoint(5, 5));
				else {
					BMessage message(kMsgLocation);
					window->PostMessage(&message, fView);
				}
				window->Unlock();
			}
			if (fQuery != NULL) {
				// Type a query into the location window and press Search.
				snooze(2000000);
				BLooper* looper = NULL;
				fView->fLocationWindow.Target(&looper);
				LocationWindow* location = dynamic_cast<LocationWindow*>(looper);
				if (location != NULL && location->Lock()) {
					location->fQuery->SetText(fQuery);
					location->PostMessage(kMsgSearch);
					location->Unlock();
				}
				if (fPick) {
					snooze(6000000);
					location->PostMessage(kMsgPick);
				}
			}
		}
	}

private:
	const char*		fMode;
	const char*		fQuery;
	bool			fPick;
	WeatherView*	fView;
};


int
main(int argc, char** argv)
{
	TestApp app(argc > 1 ? argv[1] : "glyphs", argc > 2 ? argv[2] : NULL,
		argc > 3 && strcmp(argv[3], "pick") == 0);
	app.Run();
	return 0;
}
