// rweather - a Deskbar tray weather item for Haiku.
//
// It shows the current weather for one place in the Deskbar tray, next to the
// clock: a small drawn glyph (sun, moon, cloud, rain, snow, thunder, fog) and
// the temperature. Clicking it opens a menu with the details and a three-day
// forecast. Like rtemperature, the executable adds a replicant to the tray and
// quits, so everything lives inside Deskbar and nothing is left in the team
// list; Deskbar persists its shelf, so the item comes back after a reboot.
//
// The data comes from Open-Meteo (https://open-meteo.com), which needs no API
// key. It is fetched over plain HTTP, not HTTPS, and that is deliberate: on an
// x86 hybrid Haiku this add-on has to be a gcc2 binary (see below), and the
// gcc2 build of libbnetapi has no TLS -- BSecureSocket::Connect() returns
// B_NOT_SUPPORTED there. Open-Meteo still answers on port 80. Nothing sent or
// received is private: a latitude, a longitude and a public forecast.
//
// Every network request runs on its own thread and reports back with a
// BMessage. Deskbar's window thread is never blocked on the network; a stalled
// request would otherwise freeze the whole Deskbar.
//
// IMPORTANT - build with the PRIMARY architecture. On an x86 hybrid Haiku,
// Deskbar is the x86_gcc2 binary and can only load add-ons of the same
// architecture, so this must be built with the gcc2 compiler (the default
// `g++`). The code is kept gcc2-compatible for that reason: no C++11, no STL.
//
// Build:  make
// Use:    ./rweather            add it to the tray
//         ./rweather --remove   take it back out
//         ./rweather --print    fetch once, print the weather and exit

#include <Application.h>
#include <Button.h>
#include <Deskbar.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <NetworkAddress.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <ScrollView.h>
#include <Socket.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>
#include <View.h>
#include <Window.h>

#include <image.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define APP_SIGNATURE	"application/x-vnd.rainygirl-RWeather"
#define REPLICANT_NAME	"rweather"
#define DEGREE			"\xc2\xb0"

// Every weather glyph is one solid color, like the other tray icons.
static const rgb_color kGlyphColor = { 0, 0, 0, 255 };

static const uint32 kMsgTick = 'tick';
static const uint32 kMsgWeather = 'wthr';
static const uint32 kMsgRefresh = 'rfsh';
static const uint32 kMsgLocation = 'loca';
static const uint32 kMsgSetLocation = 'sloc';
static const uint32 kMsgCelsius = 'cels';
static const uint32 kMsgFahrenheit = 'fahr';
static const uint32 kMsgRemove = 'rmov';
static const uint32 kMsgSearch = 'srch';
static const uint32 kMsgSearchDone = 'srdn';
static const uint32 kMsgPick = 'pick';

// The tick only decides whether a fetch is due; the intervals are below.
static const bigtime_t kTickInterval = 60000000LL;
static const bigtime_t kRefreshInterval = 15 * 60000000LL;
static const bigtime_t kRetryInterval = 2 * 60000000LL;
static const bigtime_t kNetworkTimeout = 10000000LL;

static const int kForecastDays = 4;
static const int kMaxResults = 10;


// ---- settings ---------------------------------------------------------------

struct Settings {
	double	latitude;
	double	longitude;
	bool	fahrenheit;
	char	name[128];
};


static void
DefaultSettings(Settings* settings)
{
	settings->latitude = 37.566;
	settings->longitude = 126.9784;
	settings->fahrenheit = false;
	strcpy(settings->name, "Seoul");
}


static bool
SettingsPath(BPath* path)
{
	if (find_directory(B_USER_SETTINGS_DIRECTORY, path) != B_OK)
		return false;
	return path->Append("RWeather") == B_OK;
}


// A plain key=value text file, so it can be read and edited by hand.
static void
LoadSettings(Settings* settings)
{
	DefaultSettings(settings);

	BPath path;
	if (!SettingsPath(&path))
		return;
	FILE* file = fopen(path.Path(), "r");
	if (file == NULL)
		return;

	char line[256];
	while (fgets(line, sizeof(line), file) != NULL) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "lat=", 4) == 0)
			settings->latitude = atof(line + 4);
		else if (strncmp(line, "lon=", 4) == 0)
			settings->longitude = atof(line + 4);
		else if (strncmp(line, "unit=", 5) == 0)
			settings->fahrenheit = line[5] == 'F';
		else if (strncmp(line, "name=", 5) == 0 && line[5] != '\0') {
			strncpy(settings->name, line + 5, sizeof(settings->name) - 1);
			settings->name[sizeof(settings->name) - 1] = '\0';
		}
	}
	fclose(file);
}


static void
SaveSettings(const Settings& settings)
{
	BPath path;
	if (!SettingsPath(&path))
		return;
	FILE* file = fopen(path.Path(), "w");
	if (file == NULL)
		return;
	fprintf(file, "lat=%.4f\nlon=%.4f\nunit=%c\nname=%s\n", settings.latitude,
		settings.longitude, settings.fahrenheit ? 'F' : 'C', settings.name);
	fclose(file);
}


// ---- HTTP -------------------------------------------------------------------

// One blocking HTTP/1.0 GET. HTTP/1.0 keeps the body unchunked, and the
// server closes the connection when it is done, so the body is simply
// everything after the headers.
static bool
HttpGet(const char* host, const char* path, BString* body, BString* error)
{
	BNetworkAddress address;
	if (address.SetTo(host, 80) != B_OK) {
		*error = "Cannot resolve ";
		*error << host;
		return false;
	}

	BSocket socket;
	if (socket.Connect(address, kNetworkTimeout) != B_OK) {
		*error = "Cannot connect to ";
		*error << host;
		return false;
	}
	socket.SetTimeout(kNetworkTimeout);

	BString request;
	request << "GET " << path << " HTTP/1.0\r\n"
		<< "Host: " << host << "\r\n"
		<< "User-Agent: RWeather/1.0 (Haiku)\r\n"
		<< "Accept: application/json\r\n"
		<< "Connection: close\r\n\r\n";

	const char* data = request.String();
	int32 left = request.Length();
	while (left > 0) {
		ssize_t written = socket.Write(data, left);
		if (written <= 0) {
			*error = "Request failed";
			return false;
		}
		data += written;
		left -= written;
	}

	BString response;
	char buffer[4096];
	for (;;) {
		ssize_t got = socket.Read(buffer, sizeof(buffer));
		if (got <= 0)
			break;
		response.Append(buffer, got);
		if (response.Length() > 1024 * 1024)
			break;
	}

	int status = 0;
	if (sscanf(response.String(), "HTTP/%*d.%*d %d", &status) != 1) {
		*error = "No response";
		return false;
	}
	int32 headerEnd = response.FindFirst("\r\n\r\n");
	if (headerEnd < 0) {
		*error = "Truncated response";
		return false;
	}
	if (status != 200) {
		error->SetTo("");
		*error << "HTTP status " << (int32)status;
		return false;
	}
	response.CopyInto(*body, headerEnd + 4, response.Length() - headerEnd - 4);
	return true;
}


static void
UrlEncode(const char* text, BString* out)
{
	static const char* kHex = "0123456789ABCDEF";
	for (const unsigned char* p = (const unsigned char*)text; *p != 0; p++) {
		unsigned char c = *p;
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			|| (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'
			|| c == '~') {
			out->Append((char)c, 1);
		} else {
			char escaped[4];
			escaped[0] = '%';
			escaped[1] = kHex[c >> 4];
			escaped[2] = kHex[c & 0x0f];
			escaped[3] = '\0';
			out->Append(escaped);
		}
	}
}


// ---- a tiny JSON extractor for the few known keys --------------------------

// Given a pointer at '{' or '[', returns the pointer just past its match.
static const char*
SkipContainer(const char* p)
{
	int depth = 0;
	bool inString = false;
	for (; *p != '\0'; p++) {
		if (inString) {
			if (*p == '\\' && p[1] != '\0')
				p++;
			else if (*p == '"')
				inString = false;
			continue;
		}
		if (*p == '"')
			inString = true;
		else if (*p == '{' || *p == '[')
			depth++;
		else if (*p == '}' || *p == ']') {
			if (--depth == 0)
				return p + 1;
		}
	}
	return p;
}


// Copies the object or array stored under "key" into *out. The quotes around
// the key keep "current" from matching "current_units".
static bool
JsonSection(const char* json, const char* key, BString* out)
{
	BString pattern;
	pattern << "\"" << key << "\":";
	const char* p = strstr(json, pattern.String());
	if (p == NULL)
		return false;
	p += pattern.Length();
	while (*p == ' ')
		p++;
	if (*p != '{' && *p != '[')
		return false;
	const char* end = SkipContainer(p);
	out->SetTo(p, end - p);
	return true;
}


static bool
JsonNumber(const char* json, const char* key, double* value)
{
	BString pattern;
	pattern << "\"" << key << "\":";
	const char* p = strstr(json, pattern.String());
	if (p == NULL)
		return false;
	p += pattern.Length();
	while (*p == ' ')
		p++;
	if (*p != '-' && (*p < '0' || *p > '9'))
		return false;
	*value = atof(p);
	return true;
}


static void
AppendUtf8(BString* out, unsigned int code)
{
	char buffer[4];
	int length;
	if (code < 0x80) {
		buffer[0] = (char)code;
		length = 1;
	} else if (code < 0x800) {
		buffer[0] = (char)(0xc0 | (code >> 6));
		buffer[1] = (char)(0x80 | (code & 0x3f));
		length = 2;
	} else {
		buffer[0] = (char)(0xe0 | (code >> 12));
		buffer[1] = (char)(0x80 | ((code >> 6) & 0x3f));
		buffer[2] = (char)(0x80 | (code & 0x3f));
		length = 3;
	}
	out->Append(buffer, length);
}


static bool
JsonString(const char* json, const char* key, BString* out)
{
	BString pattern;
	pattern << "\"" << key << "\":";
	const char* p = strstr(json, pattern.String());
	if (p == NULL)
		return false;
	p += pattern.Length();
	while (*p == ' ')
		p++;
	if (*p != '"')
		return false;
	p++;

	out->SetTo("");
	for (; *p != '\0' && *p != '"'; p++) {
		if (*p != '\\') {
			out->Append(p, 1);
			continue;
		}
		p++;
		switch (*p) {
			case 'n': out->Append("\n"); break;
			case 't': out->Append("\t"); break;
			case 'u':
			{
				char hex[5];
				strncpy(hex, p + 1, 4);
				hex[4] = '\0';
				if (strlen(hex) == 4) {
					AppendUtf8(out, (unsigned int)strtoul(hex, NULL, 16));
					p += 4;
				}
				break;
			}
			case '\0': return true;
			default: out->Append(p, 1); break;
		}
	}
	return true;
}


// Reads up to `max` numbers from the array stored under "key".
static int
JsonNumberArray(const char* json, const char* key, double* values, int max)
{
	BString array;
	if (!JsonSection(json, key, &array))
		return 0;
	const char* p = array.String() + 1;
	int count = 0;
	while (count < max && *p != '\0' && *p != ']') {
		while (*p == ' ' || *p == ',')
			p++;
		if (*p == ']' || *p == '\0')
			break;
		values[count++] = strncmp(p, "null", 4) == 0 ? 0.0 : atof(p);
		while (*p != '\0' && *p != ',' && *p != ']')
			p++;
	}
	return count;
}


// ---- weather ----------------------------------------------------------------

struct Weather {
	double	temperature;
	double	feelsLike;
	double	humidity;
	double	wind;
	int32	code;
	bool	isDay;
	int32	days;
	int32	dayCode[kForecastDays];
	double	dayMax[kForecastDays];
	double	dayMin[kForecastDays];
	char	dayName[kForecastDays][4];
	bool	fahrenheit;
};


static const char*
ConditionName(int code)
{
	switch (code) {
		case 0: return "Clear";
		case 1: return "Mainly clear";
		case 2: return "Partly cloudy";
		case 3: return "Overcast";
		case 45: return "Fog";
		case 48: return "Rime fog";
		case 51: case 53: case 55: return "Drizzle";
		case 56: case 57: return "Freezing drizzle";
		case 61: return "Light rain";
		case 63: return "Rain";
		case 65: return "Heavy rain";
		case 66: case 67: return "Freezing rain";
		case 71: return "Light snow";
		case 73: return "Snow";
		case 75: return "Heavy snow";
		case 77: return "Snow grains";
		case 80: case 81: return "Rain showers";
		case 82: return "Heavy showers";
		case 85: case 86: return "Snow showers";
		case 95: return "Thunderstorm";
		case 96: case 99: return "Thunderstorm, hail";
	}
	return "Unknown";
}


static int
Round(double value)
{
	return (int)floor(value + 0.5);
}


static void
DayName(const char* isoDate, char* out)
{
	struct tm date;
	memset(&date, 0, sizeof(date));
	strcpy(out, "---");
	if (sscanf(isoDate, "%d-%d-%d", &date.tm_year, &date.tm_mon,
			&date.tm_mday) != 3)
		return;
	date.tm_year -= 1900;
	date.tm_mon -= 1;
	date.tm_hour = 12;
	if (mktime(&date) == (time_t)-1)
		return;
	static const char* kDays[] = {
		"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
	};
	strcpy(out, kDays[date.tm_wday % 7]);
}


static bool
FetchWeather(const Settings& settings, Weather* weather, BString* error)
{
	char path[512];
	snprintf(path, sizeof(path),
		"/v1/forecast?latitude=%.4f&longitude=%.4f"
		"&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
		"is_day,weather_code,wind_speed_10m"
		"&daily=weather_code,temperature_2m_max,temperature_2m_min"
		"&timezone=auto&forecast_days=%d%s",
		settings.latitude, settings.longitude, kForecastDays,
		settings.fahrenheit
			? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");

	BString body;
	if (!HttpGet("api.open-meteo.com", path, &body, error))
		return false;

	BString current;
	BString daily;
	if (!JsonSection(body.String(), "current", &current)
		|| !JsonSection(body.String(), "daily", &daily)) {
		*error = "Unexpected response";
		return false;
	}

	memset(weather, 0, sizeof(*weather));
	double value = 0;
	if (!JsonNumber(current.String(), "temperature_2m", &weather->temperature)
		|| !JsonNumber(current.String(), "weather_code", &value)) {
		*error = "No current weather in response";
		return false;
	}
	weather->code = (int32)value;
	JsonNumber(current.String(), "apparent_temperature", &weather->feelsLike);
	JsonNumber(current.String(), "relative_humidity_2m", &weather->humidity);
	JsonNumber(current.String(), "wind_speed_10m", &weather->wind);
	value = 1;
	JsonNumber(current.String(), "is_day", &value);
	weather->isDay = value != 0;
	weather->fahrenheit = settings.fahrenheit;

	double codes[kForecastDays];
	int days = JsonNumberArray(daily.String(), "weather_code", codes,
		kForecastDays);
	int highs = JsonNumberArray(daily.String(), "temperature_2m_max",
		weather->dayMax, kForecastDays);
	int lows = JsonNumberArray(daily.String(), "temperature_2m_min",
		weather->dayMin, kForecastDays);
	if (highs < days)
		days = highs;
	if (lows < days)
		days = lows;
	weather->days = days;

	BString times;
	const char* date = NULL;
	if (JsonSection(daily.String(), "time", &times))
		date = times.String();
	for (int i = 0; i < days; i++) {
		weather->dayCode[i] = (int32)codes[i];
		strcpy(weather->dayName[i], "---");
		if (date != NULL && (date = strchr(date, '"')) != NULL) {
			DayName(date + 1, weather->dayName[i]);
			date = strchr(date + 1, '"');
			if (date != NULL)
				date++;
		}
	}
	return true;
}


struct FetchJob {
	Settings	settings;
	BMessenger	target;
};


static int32
fetch_thread(void* data)
{
	FetchJob* job = (FetchJob*)data;
	Weather weather;
	BString error;

	BMessage reply(kMsgWeather);
	if (FetchWeather(job->settings, &weather, &error))
		reply.AddData("weather", B_RAW_TYPE, &weather, sizeof(weather));
	else
		reply.AddString("error", error.String());
	job->target.SendMessage(&reply);

	delete job;
	return 0;
}


// ---- location search window ---------------------------------------------------

struct SearchJob {
	BString		query;
	BMessenger	target;
};


static int32
search_thread(void* data)
{
	SearchJob* job = (SearchJob*)data;

	BString path("/v1/search?count=10&language=en&format=json&name=");
	UrlEncode(job->query.String(), &path);

	BMessage reply(kMsgSearchDone);
	BString body;
	BString error;
	BString results;
	if (!HttpGet("geocoding-api.open-meteo.com", path.String(), &body,
			&error)) {
		reply.AddString("error", error.String());
	} else if (JsonSection(body.String(), "results", &results)) {
		// Walk the array one top-level object at a time.
		const char* p = results.String() + 1;
		int count = 0;
		while (count < kMaxResults && (p = strchr(p, '{')) != NULL) {
			const char* end = SkipContainer(p);
			BString object(p, end - p);
			p = end;

			BString name;
			BString admin;
			BString country;
			double latitude;
			double longitude;
			if (!JsonString(object.String(), "name", &name)
				|| !JsonNumber(object.String(), "latitude", &latitude)
				|| !JsonNumber(object.String(), "longitude", &longitude))
				continue;
			JsonString(object.String(), "admin1", &admin);
			JsonString(object.String(), "country", &country);

			BString label(name);
			if (admin.Length() > 0 && admin != name)
				label << ", " << admin;
			if (country.Length() > 0)
				label << ", " << country;

			reply.AddString("name", name.String());
			reply.AddString("label", label.String());
			reply.AddDouble("latitude", latitude);
			reply.AddDouble("longitude", longitude);
			count++;
		}
	}
	job->target.SendMessage(&reply);

	delete job;
	return 0;
}


class LocationWindow : public BWindow {
public:
						LocationWindow(BMessenger target);
	virtual				~LocationWindow();

	virtual void		MessageReceived(BMessage* message);

private:
	void				StartSearch();

	BMessenger			fTarget;
	BTextControl*		fQuery;
	BButton*			fSearch;
	BButton*			fOK;
	BListView*			fList;
	BStringView*		fStatus;
	BMessage			fResults;
	thread_id			fThread;
};


LocationWindow::LocationWindow(BMessenger target)
	:
	BWindow(BRect(0, 0, 380, 300), "Weather location", B_TITLED_WINDOW,
		B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS),
	fTarget(target),
	fThread(-1)
{
	fQuery = new BTextControl("query", "City:", "", new BMessage(kMsgSearch));
	fSearch = new BButton("search", "Search", new BMessage(kMsgSearch));
	fList = new BListView("results");
	fList->SetSelectionMessage(new BMessage('selc'));
	fList->SetInvocationMessage(new BMessage(kMsgPick));
	BScrollView* scroll = new BScrollView("scroll", fList, 0, false, true);
	fStatus = new BStringView("status", "Search for a city by name.");
	BButton* cancel = new BButton("cancel", "Cancel",
		new BMessage(B_QUIT_REQUESTED));
	fOK = new BButton("ok", "OK", new BMessage(kMsgPick));
	fOK->SetEnabled(false);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.AddGroup(B_HORIZONTAL)
			.Add(fQuery)
			.Add(fSearch)
		.End()
		.Add(scroll, 1.0f)
		.Add(fStatus)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(cancel)
			.Add(fOK)
		.End();

	scroll->SetExplicitMinSize(BSize(340, 160));
	fQuery->MakeFocus(true);
	CenterOnScreen();
}


LocationWindow::~LocationWindow()
{
	// The search thread runs code from this add-on; it must be gone before
	// Deskbar can unload the image.
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
	}
	for (int32 i = fList->CountItems() - 1; i >= 0; i--)
		delete fList->RemoveItem(i);
}


void
LocationWindow::StartSearch()
{
	BString query(fQuery->Text());
	query.Trim();
	if (query.Length() == 0)
		return;
	if (fThread >= 0) {
		thread_info info;
		if (get_thread_info(fThread, &info) == B_OK)
			return;
	}

	SearchJob* job = new SearchJob;
	job->query = query;
	job->target = BMessenger(this);
	fThread = spawn_thread(search_thread, "rweather search",
		B_NORMAL_PRIORITY, job);
	if (fThread < 0) {
		delete job;
		return;
	}
	fSearch->SetEnabled(false);
	fStatus->SetText("Searching...");
	resume_thread(fThread);
}


void
LocationWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSearch:
			StartSearch();
			break;

		case kMsgSearchDone:
		{
			fSearch->SetEnabled(true);
			for (int32 i = fList->CountItems() - 1; i >= 0; i--)
				delete fList->RemoveItem(i);
			fResults = *message;
			fOK->SetEnabled(false);

			const char* error;
			if (message->FindString("error", &error) == B_OK) {
				fStatus->SetText(error);
				break;
			}
			const char* label;
			for (int32 i = 0;
					message->FindString("label", i, &label) == B_OK; i++)
				fList->AddItem(new BStringItem(label));
			if (fList->CountItems() == 0)
				fStatus->SetText("No place found.");
			else {
				fStatus->SetText("Pick a place and press OK.");
				fList->Select(0);
			}
			break;
		}

		case 'selc':
			fOK->SetEnabled(fList->CurrentSelection() >= 0);
			break;

		case kMsgPick:
		{
			int32 index = fList->CurrentSelection();
			const char* name;
			double latitude;
			double longitude;
			if (index < 0
				|| fResults.FindString("name", index, &name) != B_OK
				|| fResults.FindDouble("latitude", index, &latitude) != B_OK
				|| fResults.FindDouble("longitude", index, &longitude)
					!= B_OK)
				break;
			BMessage location(kMsgSetLocation);
			location.AddString("name", name);
			location.AddDouble("latitude", latitude);
			location.AddDouble("longitude", longitude);
			fTarget.SendMessage(&location);
			PostMessage(B_QUIT_REQUESTED);
			break;
		}

		default:
			BWindow::MessageReceived(message);
	}
}


// ---- the tray view ------------------------------------------------------------

class WeatherView : public BView {
public:
						WeatherView(BRect frame);
						WeatherView(BMessage* archive);
	virtual				~WeatherView();

	static BArchivable*	Instantiate(BMessage* archive);
	virtual status_t	Archive(BMessage* archive, bool deep = true) const;

	virtual void		AttachedToWindow();
	virtual void		DetachedFromWindow();
	virtual void		Draw(BRect updateRect);
	virtual void		MouseDown(BPoint where);
	virtual void		MessageReceived(BMessage* message);
	virtual void		GetPreferredSize(float* width, float* height);

private:
	void				Init();
	void				StartFetch();
	void				UpdateToolTip();
	void				ShowMenu(BPoint where);
	void				CloseLocationWindow();
	void				RemoveFromDeskbar();
	BString				FormatTemperature(double value) const;

	void				DrawGlyph(BRect box);
	void				DrawCloud(float x, float y, rgb_color fill,
							rgb_color edge);
	void				DrawSun(float x, float y, float radius);
	void				DrawMoon(float x, float y, float radius);

	Settings			fSettings;
	Weather				fWeather;
	bool				fValid;
	BString				fError;
	bigtime_t			fLastAttempt;
	bigtime_t			fLastSuccess;
	time_t				fUpdatedAt;
	thread_id			fThread;
	BMessageRunner*		fRunner;
	BMessenger			fLocationWindow;
};


WeatherView::WeatherView(BRect frame)
	:
	BView(frame, REPLICANT_NAME, B_FOLLOW_NONE, B_WILL_DRAW)
{
	Init();
}


WeatherView::WeatherView(BMessage* archive)
	:
	BView(archive)
{
	Init();
}


void
WeatherView::Init()
{
	LoadSettings(&fSettings);
	memset(&fWeather, 0, sizeof(fWeather));
	fValid = false;
	fLastAttempt = 0;
	fLastSuccess = 0;
	fUpdatedAt = 0;
	fThread = -1;
	fRunner = NULL;
	SetViewColor(B_TRANSPARENT_COLOR);
}


WeatherView::~WeatherView()
{
	delete fRunner;
	CloseLocationWindow();
	// The fetch thread runs code from this add-on, and Deskbar unloads the
	// image right after deleting the view. The socket timeouts bound the wait.
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
	}
}


BArchivable*
WeatherView::Instantiate(BMessage* archive)
{
	if (!validate_instantiation(archive, REPLICANT_NAME))
		return NULL;
	return new WeatherView(archive);
}


status_t
WeatherView::Archive(BMessage* archive, bool deep) const
{
	status_t status = BView::Archive(archive, deep);
	if (status == B_OK)
		status = archive->AddString("add_on", APP_SIGNATURE);
	if (status == B_OK)
		status = archive->AddString("class", REPLICANT_NAME);
	return status;
}


void
WeatherView::AttachedToWindow()
{
	BView::AttachedToWindow();

	if (Parent() != NULL)
		SetLowColor(Parent()->ViewColor());
	else
		SetLowColor(ui_color(B_MENU_BACKGROUND_COLOR));

	BFont font;
	GetFont(&font);
	font.SetSize(11.0f);
	font.SetFace(B_BOLD_FACE);
	SetFont(&font);

	UpdateToolTip();
	StartFetch();

	BMessage tick(kMsgTick);
	fRunner = new BMessageRunner(BMessenger(this), &tick, kTickInterval);
}


void
WeatherView::DetachedFromWindow()
{
	delete fRunner;
	fRunner = NULL;
	CloseLocationWindow();
}


void
WeatherView::CloseLocationWindow()
{
	BLooper* looper = NULL;
	fLocationWindow.Target(&looper);
	if (looper == NULL)
		return;
	// Quit() from another thread waits for the window's thread to finish,
	// so the window is gone before the add-on image can be unloaded.
	if (looper->Lock())
		looper->Quit();
	fLocationWindow = BMessenger();
}


void
WeatherView::StartFetch()
{
	if (fThread >= 0) {
		thread_info info;
		if (get_thread_info(fThread, &info) == B_OK)
			return;
	}

	FetchJob* job = new FetchJob;
	job->settings = fSettings;
	job->target = BMessenger(this);
	fThread = spawn_thread(fetch_thread, "rweather fetch",
		B_LOW_PRIORITY, job);
	if (fThread < 0) {
		delete job;
		return;
	}
	fLastAttempt = system_time();
	resume_thread(fThread);
}


BString
WeatherView::FormatTemperature(double value) const
{
	BString text;
	text << (int32)Round(value) << DEGREE << (fWeather.fahrenheit ? "F" : "C");
	return text;
}


void
WeatherView::UpdateToolTip()
{
	BString tip(fSettings.name);
	if (fValid) {
		tip << "\n" << ConditionName(fWeather.code) << ", "
			<< FormatTemperature(fWeather.temperature);
	} else if (fError.Length() > 0)
		tip << "\n" << fError;
	else
		tip << "\nUpdating...";
	SetToolTip(tip.String());
}


void
WeatherView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgTick:
		{
			bigtime_t now = system_time();
			if (fValid && now - fLastSuccess >= kRefreshInterval)
				StartFetch();
			else if (!fValid && now - fLastAttempt >= kRetryInterval)
				StartFetch();
			break;
		}

		case kMsgWeather:
		{
			const void* data;
			ssize_t size;
			const char* error;
			if (message->FindData("weather", B_RAW_TYPE, &data, &size) == B_OK
				&& size == sizeof(Weather)) {
				memcpy(&fWeather, data, sizeof(Weather));
				fValid = true;
				fError = "";
				fLastSuccess = system_time();
				fUpdatedAt = time(NULL);
			} else if (message->FindString("error", &error) == B_OK) {
				// Keep showing the last good reading; a transient failure
				// should not blank the tray. Retry sooner, though.
				fError = error;
				if (fValid)
					fLastSuccess = system_time() - kRefreshInterval
						+ kRetryInterval;
			}
			UpdateToolTip();
			Invalidate();
			break;
		}

		case kMsgRefresh:
			StartFetch();
			break;

		case kMsgCelsius:
		case kMsgFahrenheit:
		{
			bool fahrenheit = message->what == kMsgFahrenheit;
			if (fahrenheit != fSettings.fahrenheit) {
				fSettings.fahrenheit = fahrenheit;
				SaveSettings(fSettings);
				StartFetch();
			}
			break;
		}

		case kMsgLocation:
		{
			BLooper* looper = NULL;
			fLocationWindow.Target(&looper);
			if (looper != NULL) {
				BWindow* existing = dynamic_cast<BWindow*>(looper);
				if (existing != NULL && existing->Lock()) {
					existing->Activate();
					existing->Unlock();
				}
				break;
			}
			LocationWindow* window = new LocationWindow(BMessenger(this));
			fLocationWindow = BMessenger(window);
			window->Show();
			break;
		}

		case kMsgSetLocation:
		{
			const char* name;
			double latitude;
			double longitude;
			if (message->FindString("name", &name) != B_OK
				|| message->FindDouble("latitude", &latitude) != B_OK
				|| message->FindDouble("longitude", &longitude) != B_OK)
				break;
			fSettings.latitude = latitude;
			fSettings.longitude = longitude;
			strncpy(fSettings.name, name, sizeof(fSettings.name) - 1);
			fSettings.name[sizeof(fSettings.name) - 1] = '\0';
			SaveSettings(fSettings);
			fValid = false;
			fError = "";
			UpdateToolTip();
			Invalidate();
			StartFetch();
			break;
		}

		case kMsgRemove:
			RemoveFromDeskbar();
			break;

		default:
			BView::MessageReceived(message);
	}
}


// Removing the item from inside Deskbar would unload this add-on while its
// own code is still on the stack, so launch the executable with --remove
// and let that separate team ask Deskbar instead.
void
WeatherView::RemoveFromDeskbar()
{
	image_info info;
	int32 cookie = 0;
	const char* address = (const char*)&fetch_thread;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		const char* text = (const char*)info.text;
		if (address < text || address >= text + info.text_size)
			continue;
		entry_ref ref;
		if (get_ref_for_path(info.name, &ref) == B_OK) {
			const char* argv[] = { "--remove", NULL };
			be_roster->Launch(&ref, 1, (char**)argv);
		}
		return;
	}
}


void
WeatherView::ShowMenu(BPoint where)
{
	BPopUpMenu* menu = new BPopUpMenu("weather", false, false);
	menu->SetFont(be_plain_font);

	BMenuItem* item;
	BString text(fSettings.name);
	menu->AddItem(item = new BMenuItem(text.String(), NULL));
	item->SetEnabled(false);

	if (fValid) {
		text = ConditionName(fWeather.code);
		text << ", " << FormatTemperature(fWeather.temperature);
		menu->AddItem(item = new BMenuItem(text.String(), NULL));
		item->SetEnabled(false);

		text = "Feels like ";
		text << FormatTemperature(fWeather.feelsLike);
		menu->AddItem(item = new BMenuItem(text.String(), NULL));
		item->SetEnabled(false);

		text = "Humidity ";
		text << (int32)Round(fWeather.humidity) << "%";
		menu->AddItem(item = new BMenuItem(text.String(), NULL));
		item->SetEnabled(false);

		text = "Wind ";
		text << (int32)Round(fWeather.wind)
			<< (fWeather.fahrenheit ? " mph" : " km/h");
		menu->AddItem(item = new BMenuItem(text.String(), NULL));
		item->SetEnabled(false);

		if (fWeather.days > 0) {
			menu->AddSeparatorItem();
			for (int i = 0; i < fWeather.days; i++) {
				text = i == 0 ? "Today" : fWeather.dayName[i];
				text << "  " << ConditionName(fWeather.dayCode[i]) << "  "
					<< (int32)Round(fWeather.dayMax[i]) << DEGREE " / "
					<< (int32)Round(fWeather.dayMin[i]) << DEGREE;
				menu->AddItem(item = new BMenuItem(text.String(), NULL));
				item->SetEnabled(false);
			}
		}

		char updated[64];
		struct tm local;
		localtime_r(&fUpdatedAt, &local);
		strftime(updated, sizeof(updated), "Updated %H:%M", &local);
		menu->AddSeparatorItem();
		menu->AddItem(item = new BMenuItem(updated, NULL));
		item->SetEnabled(false);
	}
	if (fError.Length() > 0) {
		text = "Error: ";
		text << fError;
		menu->AddItem(item = new BMenuItem(text.String(), NULL));
		item->SetEnabled(false);
	} else if (!fValid) {
		menu->AddItem(item = new BMenuItem("Updating...", NULL));
		item->SetEnabled(false);
	}

	menu->AddSeparatorItem();
	menu->AddItem(new BMenuItem("Refresh", new BMessage(kMsgRefresh)));
	menu->AddItem(new BMenuItem("Location...", new BMessage(kMsgLocation)));
	menu->AddSeparatorItem();
	menu->AddItem(item = new BMenuItem("Celsius", new BMessage(kMsgCelsius)));
	item->SetMarked(!fSettings.fahrenheit);
	menu->AddItem(item = new BMenuItem("Fahrenheit",
		new BMessage(kMsgFahrenheit)));
	item->SetMarked(fSettings.fahrenheit);
	menu->AddSeparatorItem();
	menu->AddItem(new BMenuItem("Remove from Deskbar",
		new BMessage(kMsgRemove)));
	menu->SetTargetForItems(this);

	// Asynchronous, so Deskbar's window thread keeps running while it is open.
	menu->SetAsyncAutoDestruct(true);
	BRect clickRect = ConvertToScreen(Bounds());
	menu->Go(ConvertToScreen(where), true, true, clickRect, true);
}


void
WeatherView::MouseDown(BPoint where)
{
	ShowMenu(where);
}


void
WeatherView::GetPreferredSize(float* width, float* height)
{
	if (width != NULL)
		*width = 46.0f;
	if (height != NULL)
		*height = 15.0f;
}


// ---- glyphs, drawn on a 16x16 grid ----------------------------------------------

void
WeatherView::DrawSun(float x, float y, float radius)
{
	SetPenSize(1.2f);
	SetHighColor(kGlyphColor);
	for (int i = 0; i < 8; i++) {
		double angle = i * M_PI / 4.0;
		float c = (float)cos(angle);
		float s = (float)sin(angle);
		StrokeLine(BPoint(x + c * (radius + 1.5f), y + s * (radius + 1.5f)),
			BPoint(x + c * (radius + 3.0f), y + s * (radius + 3.0f)));
	}
	SetPenSize(1.0f);
	FillEllipse(BPoint(x, y), radius, radius);
}


void
WeatherView::DrawMoon(float x, float y, float radius)
{
	SetHighColor(kGlyphColor);
	FillEllipse(BPoint(x, y), radius, radius);
	// Bite the crescent out with the tray's own background.
	SetHighColor(LowColor());
	FillEllipse(BPoint(x + radius * 0.55f, y - radius * 0.45f),
		radius * 0.85f, radius * 0.85f);
}


// A cloud 14 wide and 8 tall with its top-left corner at (x, y).
void
WeatherView::DrawCloud(float x, float y, rgb_color fill, rgb_color edge)
{
	for (int pass = 0; pass < 2; pass++) {
		float grow = pass == 0 ? 1.0f : 0.0f;
		SetHighColor(pass == 0 ? edge : fill);
		FillEllipse(BPoint(x + 4.0f, y + 5.0f), 3.0f + grow, 3.0f + grow);
		FillEllipse(BPoint(x + 8.0f, y + 3.5f), 3.8f + grow, 3.5f + grow);
		FillEllipse(BPoint(x + 11.0f, y + 5.5f), 2.5f + grow, 2.5f + grow);
		FillRoundRect(BRect(x + 1.0f - grow, y + 4.5f - grow,
			x + 13.5f + grow, y + 8.0f + grow), 2.0f, 2.0f);
	}
}


void
WeatherView::DrawGlyph(BRect box)
{
	float x = box.left;
	float y = box.top;
	int code = fWeather.code;
	bool day = fWeather.isDay;

	// Solid black silhouettes. Clouds get a one pixel ring of the tray
	// background, which keeps them apart from a sun, moon or bolt they overlap.
	rgb_color fill = kGlyphColor;
	rgb_color edge = LowColor();

	SetDrawingMode(B_OP_OVER);
	SetPenSize(1.0f);

	if (!fValid) {
		DrawCloud(x + 1, y + 4, fill, edge);
		return;
	}

	if (code <= 1) {
		if (day)
			DrawSun(x + 8, y + 8, 3.3f);
		else
			DrawMoon(x + 8, y + 8, 5.0f);
	} else if (code == 2) {
		if (day)
			DrawSun(x + 5, y + 5, 2.4f);
		else
			DrawMoon(x + 5.5f, y + 5, 3.8f);
		DrawCloud(x + 2, y + 6, fill, edge);
	} else if (code == 3) {
		DrawCloud(x + 1, y + 4, fill, edge);
	} else if (code == 45 || code == 48) {
		SetPenSize(1.6f);
		SetHighColor(fill);
		StrokeLine(BPoint(x + 2, y + 5), BPoint(x + 13, y + 5));
		StrokeLine(BPoint(x + 4, y + 8), BPoint(x + 15, y + 8));
		StrokeLine(BPoint(x + 1, y + 11), BPoint(x + 12, y + 11));
		SetPenSize(1.0f);
	} else if (code >= 71 && code <= 77 || code == 85 || code == 86) {
		DrawCloud(x + 1, y + 1, fill, edge);
		SetHighColor(fill);
		FillEllipse(BPoint(x + 4.5f, y + 12.5f), 1.2f, 1.2f);
		FillEllipse(BPoint(x + 8.0f, y + 14.5f), 1.2f, 1.2f);
		FillEllipse(BPoint(x + 11.5f, y + 12.5f), 1.2f, 1.2f);
	} else if (code >= 95) {
		DrawCloud(x + 1, y + 1, fill, edge);
		BPoint bolt[] = {
			BPoint(x + 9, y + 8), BPoint(x + 5.5f, y + 12.5f),
			BPoint(x + 8, y + 12.5f), BPoint(x + 6.5f, y + 16),
			BPoint(x + 11.5f, y + 11), BPoint(x + 9, y + 11),
			BPoint(x + 10.5f, y + 8)
		};
		// The background ring cuts the bolt out of the cloud it overlaps.
		SetHighColor(edge);
		StrokePolygon(bolt, 7);
		SetHighColor(fill);
		FillPolygon(bolt, 7);
	} else {
		// Drizzle, rain and showers.
		DrawCloud(x + 1, y + 1, fill, edge);
		SetPenSize(1.3f);
		SetHighColor(fill);
		float drop = code <= 57 ? 1.8f : 3.0f;
		StrokeLine(BPoint(x + 5, y + 11.5f), BPoint(x + 5 - drop / 2,
			y + 11.5f + drop));
		StrokeLine(BPoint(x + 8.5f, y + 11.5f), BPoint(x + 8.5f - drop / 2,
			y + 11.5f + drop));
		StrokeLine(BPoint(x + 12, y + 11.5f), BPoint(x + 12 - drop / 2,
			y + 11.5f + drop));
		SetPenSize(1.0f);
	}
}


void
WeatherView::Draw(BRect updateRect)
{
	FillRect(updateRect, B_SOLID_LOW);

	BRect bounds = Bounds();
	float top = floor((bounds.Height() - 15.0f) / 2.0f);
	DrawGlyph(BRect(0, top, 15, top + 15));

	BString text;
	if (fValid)
		text << (int32)Round(fWeather.temperature) << DEGREE;
	else
		text = "--";

	SetHighColor(ui_color(B_MENU_ITEM_TEXT_COLOR));
	SetDrawingMode(B_OP_OVER);

	font_height fh;
	GetFontHeight(&fh);
	float y = (bounds.Height() - (fh.ascent + fh.descent)) / 2 + fh.ascent;
	DrawString(text.String(), BPoint(19.0f, y));
}


// ---- the Deskbar add-on entry point ---------------------------------------------

extern "C" _EXPORT BView* instantiate_deskbar_item(float maxWidth,
	float maxHeight);

extern "C" _EXPORT BView*
instantiate_deskbar_item(float maxWidth, float maxHeight)
{
	(void)maxWidth;
	(void)maxHeight;
	return new WeatherView(BRect(0, 0, 45, 15));
}


// ---- the application: add the replicant, then get out of the way ------------

class App : public BApplication {
public:
	App()
		:
		BApplication(APP_SIGNATURE),
		fRemove(false)
	{
	}

	void SetRemove(bool remove) { fRemove = remove; }

	virtual void ReadyToRun()
	{
		BDeskbar deskbar;

		if (deskbar.HasItem(REPLICANT_NAME))
			deskbar.RemoveItem(REPLICANT_NAME);

		if (!fRemove) {
			app_info info;
			if (be_app->GetAppInfo(&info) == B_OK) {
				int32 id;
				if (deskbar.AddItem(&info.ref, &id) != B_OK)
					fprintf(stderr, "could not add the tray item\n");
			}
		}
		Quit();
	}

private:
	bool fRemove;
};


static int
PrintWeather()
{
	Settings settings;
	LoadSettings(&settings);

	Weather weather;
	BString error;
	if (!FetchWeather(settings, &weather, &error)) {
		fprintf(stderr, "%s: %s\n", settings.name, error.String());
		return 1;
	}

	const char* unit = weather.fahrenheit ? DEGREE "F" : DEGREE "C";
	printf("%s (%.4f, %.4f)\n", settings.name, settings.latitude,
		settings.longitude);
	printf("%s, %d%s, feels like %d%s, humidity %d%%, wind %d %s, %s\n",
		ConditionName(weather.code), Round(weather.temperature), unit,
		Round(weather.feelsLike), unit, Round(weather.humidity),
		Round(weather.wind), weather.fahrenheit ? "mph" : "km/h",
		weather.isDay ? "day" : "night");
	for (int i = 0; i < weather.days; i++) {
		printf("%s  %s  %d / %d%s\n", i == 0 ? "Today" : weather.dayName[i],
			ConditionName(weather.dayCode[i]), Round(weather.dayMax[i]),
			Round(weather.dayMin[i]), unit);
	}
	return 0;
}


int
main(int argc, char** argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--print") == 0)
			return PrintWeather();
	}

	App app;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--remove") == 0)
			app.SetRemove(true);
	}
	app.Run();
	return 0;
}
