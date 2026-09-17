# R Weather - development notes

A Deskbar tray weather item. One source file, `rweather.cpp`, is both the
application (`main`) and the Deskbar add-on (`instantiate_deskbar_item`). The
executable adds the replicant to the tray and quits; the item then lives inside
Deskbar's process, and Deskbar restores it after a reboot as long as the
executable stays where it was added from.

## What it shows

- Tray: a 16x16 glyph (sun, moon at night, partly cloudy, overcast, fog, rain,
  showers, drizzle, snow, thunder) and the rounded temperature. The number is
  drawn in `B_MENU_ITEM_TEXT_COLOR`.
- Glyphs are solid black silhouettes (`kGlyphColor`). Clouds are drawn over a
  one pixel ring of the tray background (`LowColor()`), which keeps them apart
  from the sun, moon or lightning bolt they overlap. The earlier colored/grey
  glyphs were hard to read on the light Deskbar.
- Tooltip: location, condition, temperature.
- Click: a menu with condition, feels like, humidity, wind, today's high/low,
  the next three days, Refresh, Location..., Celsius/Fahrenheit, Remove from
  Deskbar.

## gcc2 only on x86 hybrids

On an x86_gcc2 hybrid Deskbar is a gcc2 binary and only loads add-ons of its
own architecture, so this must be built with the default `g++` (gcc 2.95).
A gcc13 build makes Deskbar crash when it loads the replicant. The code stays
C++98/gcc2-compatible for that reason (BString, plain C strings, no STL).
On x86_64 and arm64 there is one architecture and the normal compiler is fine.

## Why plain HTTP

The gcc2 `libbnetapi` has no TLS: `BSecureSocket::Connect()` returns
`Operation not supported` (checked on hrev99002). Open-Meteo answers on port
80, so requests go out as HTTP/1.0 over `BSocket` (HTTP/1.0 avoids chunked
transfer encoding, so the parser stays trivial). What is sent is only latitude
and longitude, or the city name while searching; what comes back is a public
forecast.

All network work runs on spawned threads (`fetch_thread`, `search_thread`) and
reports back to the view with a BMessenger, so Deskbar's window thread never
waits on the network. Refresh every 15 minutes; after a failure retry in 2
minutes and keep showing the last good reading.

Endpoints:
- `api.open-meteo.com/v1/forecast?latitude=..&longitude=..&current=temperature_2m,apparent_temperature,relative_humidity_2m,is_day,weather_code,wind_speed_10m&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=4`
  (+ `&temperature_unit=fahrenheit`)
- `geocoding-api.open-meteo.com/v1/search?name=..&count=10&language=en`

JSON is read with a small hand-written extractor for these known keys.

Weather data by Open-Meteo.com, CC BY 4.0 - keep the attribution in the README.

## Location and settings

Default location is Seoul (37.5660, 126.9784). Location... opens a small window:
type a city, Search, pick a result, OK. The geocoder finds English and local
Latin-script names (`Seoul`, `Busan`, `Tokyo`) but returns nothing for Hangul
such as `서울`, even though the query is UTF-8 percent-encoded correctly (the
same query from a Mac is empty too).

Settings are a plain text file, `~/config/settings/RWeather`:

```
lat=37.5660
lon=126.9784
unit=C
name=Seoul
```

## Building and running

```
make                  # rweather
./rweather            # add to the tray
./rweather --remove   # remove from the tray
./rweather --print    # fetch once, print, exit (no Deskbar involved)
make deploy           # remove, copy to ~/config/non-packaged/bin, add from there
```

Deskbar memory-maps the executable while the replicant is loaded. Overwriting
the loaded file in place corrupts the running copy and crashes Deskbar, so
`make deploy` removes the item first.

## Testing

`tools/testwin.cpp` hosts WeatherView instances in an ordinary window so the
glyphs, the menu and the location window can be checked with a screenshot
without touching Deskbar:

```
g++ -Wno-multichar -o testwin tools/testwin.cpp -lbe -lbnetapi -lnetwork
./testwin glyphs          # all glyphs; the test codes replace the real fetch after 8 s
./testwin menu
./testwin location Tokyo pick
```

Menu actions inside Deskbar itself were exercised with `hey` (unit toggle,
Location window, Remove while the Location window is open) without Deskbar
restarting.

## Packaging

`pkgman-repo/recipes/rweather.recipe` installs `bin/rweather`; the user runs
`rweather` once to add the tray item, like `rtemperature`.

`tools/make_icon.py` generates the vector icon in `rweather.rdef`.
