[![Language](https://img.shields.io/badge/language-C-green.svg?style=for-the-badge)](https://ecma-international.org/publications-and-standards/standards/ecma-262/)
[![MIT](https://shields.io/badge/license-MIT-green?style=for-the-badge)](https://choosealicense.com/licenses/mit/)
[![Issues](https://img.shields.io/github/issues/jonosur/weather?style=for-the-badge)](https://github.com/jonosur/weather/issues)

### WEATHER ATHEME-SERVICES MODULE

Make sure you have curl, jannson and math libs installed.

```
sudo apt update
sudo apt install libcurl4-openssl-dev libjansson-dev libm-dev
```
Make sure you add weather directory to your Makefile.

The atheme.conf should look like this.
```
loadmodule "modules/weather/main";
weather {
        /* (*)nick
         * Sets the nick used for InfoServ and sending out informational messages.
         */
        nick = "Weather";

        /* (*)user
         * Sets the username used for this client.
         */
        user = "Weather";

        /* (*)host
         * The hostname used for this client,
         */
        host = "services.int";

        /* (*)real
         * The GECOS (real name) of the client.
         */
        real = "Weather Service";
};
```

Here is a sample of the Weather help, and outputs.

```
***** Weather Help *****
Weather provides weather information and related commands, using
OpenCage and PirateWeather as sources.
 
For more information on a command, type:
/msg Weather HELP <command>
 
The following commands are available:
FORECAST       Fetches forecast data for a location.
HELP           Displays contextual help information.
INFO           Displays user-specific weather settings information.
JOIN           Weather will join channel.
SETCOLORS      Enables or disables weather colors output.
SETGREET       Enables or disables weather greeting on identify.
SETWEATHER     Sets the default weather location for the user.
WEATHER        Fetches weather data for a location.
 
W and F shortcuts for are also available for the weather and forecast.
 
***** End of Help *****
```

```
<Weather> PPG Paints Arena, 1001 Fifth Avenue, Pittsburgh, PA 15219, United States of America :: Cloudy 35.1F/1.7C | Feels Like: 27.3F/-2.6C | Humidity: 85% | Wind: 7.2mph/11.5km/h WNW Gust: 18.0mph/28.9km/h | Dew: 32° | UV Index: 0.0 Risk: Low | Sunrise: 07:39 AM EST Sunset: 04:55 PM EST | Fri: Cloudy ↓27.4F/-2.6C ↑39.7F/4.3C | Sat: Partly Cloudy ↓19.7F/-6.8C ↑31.6F/-0.2C
```
![weather](https://i.imgur.com/hNRAY4Q.png)
