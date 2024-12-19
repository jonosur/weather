#include "atheme.h"
#include <curl/curl.h>
#include <jansson.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdbool.h>

#define OPENCAGE_URL "https://api.opencagedata.com/geocode/v1/json?q=%s&key=%s&language=en&pretty=1"
#define OPENCAGE_KEY "OPENCAGE_API_KEY_GOES_HERE"

#define PIRATE_URL "https://api.pirateweather.net/forecast"
#define PIRATE_KEY "PIRATEWEATHER_API_KEY_GOES_HERE"

#define OUTPUT_SIZE 7000
#define FORECAST_SIZE 7000
#define RATE_LIMIT_INTERVAL 60

#define DEBUG_MODE 1

DECLARE_MODULE_V1
(
    "weather/main", false, _modinit, _moddeinit,
    PACKAGE_STRING,
    VENDOR_STRING
);

void split_command_location(const char *input, char *command, char *location) {
    const char *space = strchr(input, ' ');
    if (space) {
        size_t command_len = space - input;
        strncpy(command, input, command_len);
        command[command_len] = '\0';

        strcpy(location, space + 1);
    } else {
        // If there's no space, the whole input is the command and location is empty
        strcpy(command, input);
        location[0] = '\0';
    }
}



service_t *weather;


typedef struct {
    time_t last_request_time;
    int hit_count;
} weather_service_ratelimit_t;


typedef struct {
    int hitvalue;
} weather_service_setlimit_t;

typedef struct {
        char *channel;
        char *requester;
} channel_info_t;

static weather_service_setlimit_t set_limit;
void rate_limit_free(const char *key, void *data, void *privdata)
	{
	free(data);
}
void channel_info_free(const char *key, void *data, void *privdata)
        {
        channel_info_t *ci = data;
	free(ci->channel);
	free(ci->requester);
	free(ci);
}
// Hash table to store rate limits for users
mowgli_patricia_t *rate_limit_table;
mowgli_patricia_t *channel_table;

// Function to initialize the rate limit table
void init_rate_limit() {
    rate_limit_table = mowgli_patricia_create(strcasecanon);
}

void init_channel_table() {
    channel_table = mowgli_patricia_create(strcasecanon);
}




bool check_rate_limit(sourceinfo_t *si) {
    if (!rate_limit_table) {
        slog(LG_DEBUG, "Rate limit table is not initialized.\n");
        return false;
    }

    weather_service_ratelimit_t *rate_limit = mowgli_patricia_retrieve(rate_limit_table, si->su->nick);
    time_t current_time = time(NULL);

    if (rate_limit) {
        double time_diff = difftime(current_time, rate_limit->last_request_time);
        if (time_diff < RATE_LIMIT_INTERVAL / set_limit.hitvalue) {
            rate_limit->hit_count++;
            slog(LG_DEBUG, "New rate limit entry added for user: %s (count %d)", si->su->nick, rate_limit->hit_count);
            if (rate_limit->hit_count > set_limit.hitvalue) {
                command_fail(si, fault_toomany, "You are making requests too quickly. Please wait before trying again.");
                return false;
            }
        } else {
            rate_limit->hit_count = 1;
        }
        rate_limit->last_request_time = current_time;
    } else {
        rate_limit = malloc(sizeof(weather_service_ratelimit_t));
        if (!rate_limit) {
            slog(LG_DEBUG, "Failed to allocate memory for rate limit.\n");
            return false;
        }
        rate_limit->last_request_time = current_time;
        rate_limit->hit_count = 1;
        mowgli_patricia_add(rate_limit_table, si->su->nick, rate_limit);
        slog(LG_DEBUG, "New rate limit entry added for user: %s (count %d)", si->su->nick, rate_limit->hit_count);
    }

    return true;
}



static void on_channel_message(hook_cmessage_data_t *data);
static void ws_cmd_help(sourceinfo_t *si, const int parc, char *parv[]);
static void ws_cmd_weather(sourceinfo_t *si, int parc, char *parv[]);
static void ws_cmd_forecast(sourceinfo_t *si, int parc, char *parv[]);
static void ws_cmd_setweather(sourceinfo_t *si, int parc, char *parv[]);
static void ws_cmd_setgreet(sourceinfo_t *si, int parc, char *parv[]);
static void ws_cmd_setcolors(sourceinfo_t *si, int parc, char *parv[]);
static void ws_cmd_setratelimit(sourceinfo_t *si, int parc, char *parv[]);
static void ws_cmd_info(sourceinfo_t *si, int parc, char *parv[]);
static void ws_cmd_cycle(sourceinfo_t *si, int parc, char *parv[]);
static void ws_cmd_join(sourceinfo_t *si, int parc, char *parv[]);
static char *fetch_weather_data(const char *location, const char *latlong, int forecast);
static void on_user_identify(user_t *u);

void remove_colors(char *str) {
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '\3') {
            src++; // Skip the control code

            // Skip potential color numbers (0-99)
            if (*src >= '0' && *src <= '9') src++;
            if (*src >= '0' && *src <= '9') src++;

            // Skip the comma and background color numbers, if present
            if (*src == ',') {
                src++;
                if (*src >= '0' && *src <= '9') src++;
                if (*src >= '0' && *src <= '9') src++;
            }
        } else {
            // Copy regular characters
            *dst++ = *src++;
        }
    }
    *dst = '\0'; // Null-terminate the modified string
}



const char* format_temp(const char *displaymode, double f, double c, char *buffer, size_t buffer_size) {
    char color[4];

    if (f > 100) {
        strcpy(color, "04");  // Red
    } else if (f > 85) {
        strcpy(color, "07");  // Orange
    } else if (f > 75) {
        strcpy(color, "08");  // Yellow
    } else if (f > 60) {
        strcpy(color, "09");  // Light Green
    } else if (f > 40) {
        strcpy(color, "11");  // Cyan
    } else if (f > 10) {
        strcpy(color, "12");  // Light Blue
    } else {
        strcpy(color, "15");  // Light Grey
    }

    char f_str[10], c_str[10];
    snprintf(f_str, sizeof(f_str), "%.1f", f);
    snprintf(c_str, sizeof(c_str), "%.1f", c);

    if (strcmp(displaymode, "F/C") == 0) {
        snprintf(buffer, buffer_size, "\003%s%sF/%sC\003", color, f_str, c_str);
    } else if (strcmp(displaymode, "L") == 0) {
        snprintf(buffer, buffer_size, "\003%s\2↓\2%sF/%sC\003", color, f_str, c_str);
    } else if (strcmp(displaymode, "H") == 0) {
        snprintf(buffer, buffer_size, "\003%s\2↑\2%sF/%sC\003", color, f_str, c_str);
    } else {
        return "Unknown display mode";
    }
    return buffer;
}



command_t ws_info = { "INFO", N_("Displays user-specific weather settings information."), AC_AUTHENTICATED, 1, ws_cmd_info, { .path = "weather/info" } };
command_t ws_weather = { "WEATHER", N_("Fetches weather data for a location."), AC_NONE, 1, ws_cmd_weather, { .path = "weather/weather" } };
command_t ws_w = { "W", N_("Shortcut for weather command."), AC_NONE, 1, ws_cmd_weather, { .path = "weather/weather" } };
command_t ws_forecast = { "FORECAST", N_("Fetches forecast data for a location."), AC_NONE, 1, ws_cmd_forecast, { .path = "weather/forecast" } };
command_t ws_f = { "F", N_("Fetches forecast data for a location."), AC_NONE, 1, ws_cmd_forecast, { .path = "weather/forecast" } };
command_t ws_setweather = { "SETWEATHER", N_("Sets the default weather location for the user."), AC_AUTHENTICATED, 1, ws_cmd_setweather, { .path = "weather/setweather" } };
command_t ws_setgreet = { "SETGREET", N_("Enables or disables weather greeting on identify."), AC_AUTHENTICATED, 1, ws_cmd_setgreet, { .path = "weather/setgreet" } };
command_t ws_setcolors = { "SETCOLORS", N_("Enables or disables weather colors output."), AC_AUTHENTICATED, 1, ws_cmd_setcolors, { .path = "weather/setcolors" } };
command_t ws_help = { "HELP", N_("Displays contextual help information."), AC_NONE, 1, ws_cmd_help, { .path = "help" } };
command_t ws_setratelimit = { "SETRATELIMIT", N_("Sets the rate limit for weather commands."), PRIV_ADMIN, 20, ws_cmd_setratelimit, { .path = "weather/setratelimit" } };
command_t ws_cycle = { "CYCLE", N_("Forces re-join of weather to stored channels."), PRIV_ADMIN, 20, ws_cmd_cycle, { .path = "weather/cycle" } };
command_t ws_join = { "JOIN", N_("Weather joins the channel.."), AC_NONE, 1, ws_cmd_join, { .path = "weather/join" } };

typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

typedef struct {
    char location[100];
    char latlong[100];
    int error_code;
} OpenCage;

void replace_spaces_with_underscores(char *str) {
    for (int i = 0; i < strlen(str); i++) {
        if (str[i] == ' ') {
            str[i] = '_';
        }
    }
}

struct tm* convert_to_eastern_time(time_t rawtime) {
    struct tm *timeinfo = gmtime(&rawtime);
    timeinfo->tm_hour -= 5; // Adjust for Eastern Time (UTC-5)
    mktime(timeinfo);       // Normalize the time structure
    return timeinfo;
}

const char* format_uv(double uv, char** color) {
    if (uv <= 2.9) {
        *color = "\00303"; // Green
        return "Low";
    } else if (uv <= 5.9) {
        *color = "\00308"; // Yellow
        return "Moderate";
    } else if (uv <= 7.9) {
        *color = "\00307"; // Orange
        return "High";
    } else if (uv <= 10.9) {
        *color = "\00304"; // Red
        return "Very high";
    } else {
        *color = "\00306"; // Pink
        return "Extreme";
    }
}


const char* wind_direction(int angle) {
    const char* directions[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    int num_directions = sizeof(directions) / sizeof(directions[0]);

    if (angle < 0 || angle >= 360) {
        return directions[0];
    }

    int idx = (int)((angle / (360.0 / num_directions)) + 0.5) % num_directions;
    return directions[idx];
}

size_t weather_write_callback(void *ptr, size_t size, size_t nmemb, void *data) {
    size_t real_size = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)data;

    char *ptr_realloc = realloc(mem->memory, mem->size + real_size + 1);
    if (ptr_realloc == NULL) {
        slog(LG_DEBUG, "Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr_realloc;
    memcpy(&(mem->memory[mem->size]), ptr, real_size);
    mem->size += real_size;
    mem->memory[mem->size] = 0;

    return real_size;
}

size_t opencage_write_callback(void *ptr, size_t size, size_t nmemb, void *data) {
    size_t real_size = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)data;

    char *ptr_realloc = realloc(mem->memory, mem->size + real_size + 1);
    if (ptr_realloc == NULL) {
        slog(LG_DEBUG, "Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr_realloc;
    memcpy(&(mem->memory[mem->size]), ptr, real_size);
    mem->size += real_size;
    mem->memory[mem->size] = 0;

    return real_size;
}

OpenCage fetch_geocode_data(const char *city) {
    CURL *curl;
    CURLcode res;
    MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    if (!chunk.memory) {
        slog(LG_DEBUG, "Memory allocation failed\n");
        OpenCage result = {"Memory allocation failed!", "", 1};
        return result;
    }

    OpenCage result = {"", "", 0};
    char url[256];

    curl_global_init(CURL_GLOBAL_ALL);

    snprintf(url, sizeof(url), OPENCAGE_URL, city, OPENCAGE_KEY);
    if (DEBUG_MODE) {
         slog(LG_DEBUG, url);
    }
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, opencage_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            strncpy(result.location, "Failed to perform request!", sizeof(result.location));
            strncpy(result.latlong, "Failed to perform request!", sizeof(result.latlong));
            result.error_code = res;
            curl_easy_cleanup(curl);
            free(chunk.memory);
            curl_global_cleanup();
            return result;
        }
        curl_easy_cleanup(curl);
    } else {
        strncpy(result.location, "curl_easy_init failed!", sizeof(result.location));
        strncpy(result.latlong, "curl_easy_init failed!", sizeof(result.latlong));
        result.error_code = 1;
        free(chunk.memory);
        curl_global_cleanup();
        return result;
    }

    json_t *root;
    json_error_t error;

    root = json_loads(chunk.memory, 0, &error);
    if (!root) {
        strncpy(result.location, "Failed to parse JSON!", sizeof(result.location));
        strncpy(result.latlong, "Failed to parse JSON!", sizeof(result.latlong));
        result.error_code = 2;
        free(chunk.memory);
        curl_global_cleanup();
        return result;
    }

    json_t *results = json_object_get(root, "results");
    if (!results) {
        strncpy(result.location, "No results found in json!", sizeof(result.location));
        strncpy(result.latlong, "No results found", sizeof(result.latlong));
        result.error_code = 3;
        json_decref(root);
        free(chunk.memory);
        curl_global_cleanup();
        return result;
    }

    json_t *first_result = json_array_get(results, 0);

    const char *locationf = json_string_value(json_object_get(first_result, "formatted"));
    if (!locationf) {
        strncpy(result.location, "No location formatted found", sizeof(result.location));
        strncpy(result.latlong, "No location formatted found", sizeof(result.latlong));
        result.error_code = 4;
        json_decref(root);
        free(chunk.memory);
        curl_global_cleanup();
        return result;
    }

    json_t *geometry = json_object_get(first_result, "geometry");
    json_t *lat = json_object_get(geometry, "lat");
    json_t *lng = json_object_get(geometry, "lng");

    if (!lat || !lng) {
        strncpy(result.location, "No latlong data found!", sizeof(result.location));
        strncpy(result.latlong, "No latelong data found!", sizeof(result.latlong));
        result.error_code = 5;
        json_decref(root);
        free(chunk.memory);
        curl_global_cleanup();
        return result;
    }

    double latitude = json_number_value(lat);
    double longitude = json_number_value(lng);

    snprintf(result.location, sizeof(result.location), "%s", locationf);
    snprintf(result.latlong, sizeof(result.latlong), "%f,%f", latitude, longitude);

    json_decref(root);

    free(chunk.memory);

    curl_global_cleanup();

    return result;
}

static void ws_cmd_help(sourceinfo_t *si, int parc, char *parv[])
{
    bool is_admin = has_priv(si, PRIV_ADMIN);
    char *command = parv[0];

    if (!command) {
        command_success_nodata(si, _("***** \2%s Help\2 *****"), si->service->nick);
        command_success_nodata(si, _("\2%s\2 provides weather information and related commands, using"), si->service->nick);
        command_success_nodata(si, _("OpenCage and PirateWeather as sources."));
        command_success_nodata(si, " ");
        command_success_nodata(si, _("For more information on a command, type:"));
        command_success_nodata(si, "\2/%s%s HELP <command>\2", (ircd->uses_rcommand == false) ? "msg " : "", weather->disp);
        command_success_nodata(si, " ");
        command_success_nodata(si, "The following commands are available:");
        command_success_nodata(si, "\2FORECAST\2       Fetches forecast data for a location.");
        command_success_nodata(si, "\2HELP\2           Displays contextual help information.");
        command_success_nodata(si, "\2INFO\2           Displays user-specific weather settings information.");
        command_success_nodata(si, "\2JOIN\2           %s will join channel.", si->service->nick);
        command_success_nodata(si, "\2SETCOLORS\2      Enables or disables weather colors output.");
        command_success_nodata(si, "\2SETGREET\2       Enables or disables weather greeting on identify.");
        command_success_nodata(si, "\2SETWEATHER\2     Sets the default weather location for the user.");
        if (is_admin) {
        command_success_nodata(si, "\2SETRATELIMIT\2   Sets the global rate limit for the service.");
        command_success_nodata(si, "\2CYCLE\2          Forces %s to join stored channels.", si->service->nick);
        }
        command_success_nodata(si, "\2WEATHER\2        Fetches weather data for a location.");
        command_success_nodata(si, " ");
        command_success_nodata(si, "\2W\2 and \2F\2 shortcuts for are also available for the weather and forecast.");
        command_success_nodata(si, " ");
        command_success_nodata(si, _("***** \2End of Help\2 *****"));
        return;
    }

    help_display(si, si->service, command, si->service->commands);
}

static void ws_cmd_info(sourceinfo_t *si, int parc, char *parv[])
{
    const char *latlong = NULL;
    const char *location = NULL;
    const char *greet = NULL;
    const char *colors = NULL;
    metadata_t *md;

    join("#XYZ", "Weather");
    md = metadata_find(si->smu, "private:weather:location");
    if (md)
        location = md->value;

    md = metadata_find(si->smu, "private:weather:latlong");
    if (md)
        latlong = md->value;

    md = metadata_find(si->smu, "private:weather:greet");
    if (md)
        greet = md->value;

    md = metadata_find(si->smu, "private:weather:colors");
    if (md)
        colors = md->value;

    command_success_nodata(si, "Weather information for \2%s:\2", entity(si->smu)->name);
    if (location)
        command_success_nodata(si, " Default location: %s", location);
    else
        command_success_nodata(si, " Default location: Not set");

    if (latlong)
        command_success_nodata(si, "Default lat, long: %s", latlong);
    else
        command_success_nodata(si, "Default lat, long: Not set");

    if (greet && !strcasecmp(greet, "ON"))
        command_success_nodata(si, "    Greet setting: Enabled");
    else
        command_success_nodata(si, "    Greet setting: %s", greet);

    if (greet && !strcasecmp(greet, "ON"))
        command_success_nodata(si, "    Greet setting: Enabled");
    else
        command_success_nodata(si, "    Greet setting: %s", greet);

    if (colors && !strcasecmp(colors, "ON"))
        command_success_nodata(si, "    Colors setting: Enabled");
    else
        command_success_nodata(si, "    Colors setting: %s", greet);

}

static void ws_cmd_weather(sourceinfo_t *si, int parc, char *parv[])
{
    const char *templocation = parv[0];
    char *weather_data;
    if (!check_rate_limit(si)) {
        // Rate limit check failed
        return;
    }

    if((si->smu == NULL) && templocation == NULL) {
        command_fail(si, fault_needmoreparams, _("No location was requested or use SETWEATHER to set default location."));
        return;
        }

    metadata_t *md1;
    md1 = metadata_find(si->smu, "private:weather:location");
    if((md1 == NULL) && templocation == NULL) {
        command_fail(si, fault_needmoreparams, _("No location was requested or use SETWEATHER to set default location."));
        return;
        }
    metadata_t *md2 = metadata_find(si->smu, "private:weather:latlong");
    metadata_t *md3 = metadata_find(si->smu, "private:weather:colors");


    char location[256];
    char *latlong;
    char *colors;
    if (md1 != NULL) {
            strcpy(location, md1->value);
    }

    if (md2 != NULL) {
            latlong = md2->value;
    }

    if (md3 != NULL) {
            colors = md3->value;
    } else {
            colors = "ON";
    }

    if (!templocation) {
        if (location) {
            weather_data = fetch_weather_data(location, latlong, 0);
            slog(LG_DEBUG, weather_data);
        }
        if (colors && !strcasecmp(colors, "OFF")) {
                remove_colors(weather_data);
        } 

        slog(LG_DEBUG, colors);
        command_success_nodata(si, weather_data);
           return;
        }

    if(md1 == NULL) {
        command_fail(si, fault_needmoreparams, _("No location was requested or use SETWEATHER to set default location."));
        return;
        }
    strcpy(location, templocation);
    slog(LG_DEBUG, "%s %s", location, templocation);
    replace_spaces_with_underscores(location);
    OpenCage result = fetch_geocode_data(location);
    slog(LG_DEBUG, result.location);
    if (result.error_code == 0) {
        weather_data = fetch_weather_data(result.location, result.latlong, 0);
        if (colors && !strcasecmp(colors, "OFF")) {
                remove_colors(weather_data);
        }

        command_success_nodata(si, weather_data);
    } else {
        command_success_nodata(si, "Error: %s", result.location);
    }
}

static void ws_cmd_forecast(sourceinfo_t *si, int parc, char *parv[])
{
    const char *templocation = parv[0];
    char *weather_data;
    if (!check_rate_limit(si)) {
        // Rate limit check failed
        return;
    }
    if((si->smu == NULL) && templocation == NULL) {
        command_fail(si, fault_needmoreparams, _("No location was requested or use SETWEATHER to set default location."));
        return;
    }
    if (!templocation) {
        const char *location;
        const char *latlong;
        metadata_t *md = metadata_find(si->smu, "private:weather:location");
        if (md != NULL) {
            location = md->value;
            metadata_t *md = metadata_find(si->smu, "private:weather:latlong");
            latlong = md->value;
            weather_data = fetch_weather_data(location, latlong, 1);
            const char *colors = metadata_find(si->smu, "private:weather:colors")->value;
            if (colors && !strcasecmp(colors, "OFF")) {
                  remove_colors(weather_data);
            }
            command_success_nodata(si, weather_data);
            return;
        } else {
            command_fail(si, fault_needmoreparams, _("No location was requested or use SETWEATHER to set default location."));
            return;
        }
    }
    char location[256];
    snprintf(location, sizeof(location), "%s", templocation);
    replace_spaces_with_underscores(location);
    OpenCage result = fetch_geocode_data(location);
    if (result.error_code == 0) {
        weather_data = fetch_weather_data(result.location, result.latlong, 1);
        const char *colors = metadata_find(si->smu, "private:weather:colors")->value;
        if (colors && !strcasecmp(colors, "OFF")) {
                remove_colors(weather_data);
        }
        command_success_nodata(si, weather_data);
    } else {
        command_success_nodata(si, "Error: %s", result.location);
    }
}

static void ws_cmd_setweather(sourceinfo_t *si, int parc, char *parv[])
{
    const char *templocation = parv[0];
    char location[256];
    if (!check_rate_limit(si)) {
        // Rate limit check failed
        return;
    }
    snprintf(location, sizeof(location), "%s", templocation);

    if (!location) {
        command_fail(si, fault_needmoreparams, _("Usage: SETWEATHER <location>"));
        return;
    }
    replace_spaces_with_underscores(location);

    OpenCage result = fetch_geocode_data(location);
    if (result.error_code == 0) {
        command_success_nodata(si, "The following location was set \2%s\2", result.location);
        metadata_add(si->smu, "private:weather:location", result.location);
        metadata_add(si->smu, "private:weather:latlong", result.latlong);
    } else {
        command_success_nodata(si, "\2Error:\2 %s\2", result.location);
    }
    /* Let's avoid nulls */
    metadata_t *md1 = metadata_find(si->smu, "private:weather:greet");
    if (md1 == NULL) {
       metadata_add(si->smu, "private:weather:greet", "ON");
    }
    metadata_t *md2 = metadata_find(si->smu, "private:weather:colors");
    if (md2 == NULL) {
       metadata_add(si->smu, "private:weather:colors", "ON");
    }

}

static void ws_cmd_setgreet(sourceinfo_t *si, int parc, char *parv[])
{
    const char *option = parv[0];

    if (!option) {
        command_fail(si, fault_needmoreparams, _("Usage: SETGREET <ON|OFF>"));
        return;
    }

    if (strcasecmp(option, "ON") == 0) {
        metadata_add(si->smu, "private:weather:greet", "ON");
        command_success_nodata(si, _("Weather greeting enabled."));
    } else if (strcasecmp(option, "OFF") == 0) {
        metadata_delete(si->smu, "private:weather:greet");
        command_success_nodata(si, _("Weather greeting disabled."));
    } else {
        command_fail(si, fault_badparams, _("Usage: SETGREET <ON|OFF>"));
    }
}

static void ws_cmd_setcolors(sourceinfo_t *si, int parc, char *parv[])
{
    const char *option = parv[0];

    if (!option) {
        command_fail(si, fault_needmoreparams, _("Usage: SETCOLOR <ON|OFF>"));
        return;
    }

    if (strcasecmp(option, "ON") == 0) {
        metadata_add(si->smu, "private:weather:colors", "ON");
        command_success_nodata(si, _("Weather colors enabled."));
    } else if (strcasecmp(option, "OFF") == 0) {
        metadata_add(si->smu, "private:weather:colors", "OFF");
        command_success_nodata(si, _("Weather colors disabled."));
    } else {
        command_fail(si, fault_badparams, _("Usage: SETCOLORS <ON|OFF>"));
    }
}

static void on_user_identify(user_t *u)
{
    char *colors;
    char *greet;
    /* If the greet is null lets do nothing */
    metadata_t *md1 = metadata_find(u->myuser, "private:weather:greet");
    if (md1 == NULL) {
         return;
    } else {
       greet = md1->value;
    }
    /* If no color is set, lets keep it on */
    metadata_t *md2 = metadata_find(u->myuser, "private:weather:colors");
    if (md2 != NULL) {
        colors = md2->value;
    } else {
        colors = "ON";

    }
    if (greet && !strcasecmp(greet, "ON")) {
        const char *location = metadata_find(u->myuser, "private:weather:location")->value;
        const char *latlong = metadata_find(u->myuser, "private:weather:latlong")->value;
        /* Greet is on but let's not assume a location is set */
        if (location) {
            char *weather_data = fetch_weather_data(location, latlong, 0);
            if (colors && !strcasecmp(colors, "OFF")) {
                remove_colors(weather_data);
            }
            if (weather_data) {
                notice("Weather", u->nick, weather_data);
            } else {
                notice("Weather", u->nick, _("Failed to fetch weather data."));
            }
        }
    }
}


static void ws_cmd_setratelimit(sourceinfo_t *si, int parc, char *parv[]) {
    const char *value = parv[0];

    // Ensure the command is issued by a priv_admin
    if (!has_priv(si, PRIV_ADMIN)) {
        command_fail(si, fault_noprivs, _("You do not have the required privileges to use this command."));
        return;
    }

    if (!value) {
        command_fail(si, fault_needmoreparams, _("Usage: SETRATELIMIT <value>"));
        return;
    }
    char *endptr;
    errno = 0;
    long the_limit = strtol(value, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || the_limit < 0 || the_limit > INT_MAX) {
        command_fail(si, fault_badparams, _("Invalid rate limit value. Please provide a positive integer."));
        return;
    }
    static char output[OUTPUT_SIZE] = "";
    set_limit.hitvalue = atoi(value);
    snprintf(output, sizeof(output), "Global rate limit is now %d hits per minute", set_limit.hitvalue);
    command_success_nodata(si, output);
}



static char* fetch_weather_data(const char *location, const char *latlong, int forecast) {
    CURL *curl;
    CURLcode res;
    MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;
    slog(LG_DEBUG, "Fetching weather! BARK! BARK!");
    if (!chunk.memory) {
        slog(LG_DEBUG, "Memory allocation failed\n");
        return NULL;
    }

    static char output[OUTPUT_SIZE] = "";
    char out[100] = "";
    snprintf(output, sizeof(output), "\2%s\2 :: ",location);

    curl_global_init(CURL_GLOBAL_ALL);

    curl = curl_easy_init();
    if (curl) {
        char url[256];
        snprintf(url, sizeof(url), "%s/%s/%s", PIRATE_URL, PIRATE_KEY, latlong);
        slog(LG_DEBUG, url);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, weather_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        res = curl_easy_perform(curl);
        slog(LG_DEBUG, "CURL");
        if (res != CURLE_OK) {
            snprintf(chunk.memory, chunk.size, "Failed to fetch weather data: %s", curl_easy_strerror(res));
            slog(LG_DEBUG, "Failed to fetch weather data: %s", curl_easy_strerror(res));
            curl_easy_cleanup(curl);
            free(chunk.memory);
            curl_global_cleanup();
            return chunk.memory;
        }
        curl_easy_cleanup(curl);
    } else {
        snprintf(output, sizeof(output), "curl_easy_init failed!");
        slog(LG_DEBUG, output);
        free(chunk.memory);
        curl_global_cleanup();
        return strdup(output);
    }
    slog(LG_DEBUG, "CURL GOOD");
    json_t *wroot;
    json_error_t werror;

    wroot = json_loads(chunk.memory, 0, &werror);
    slog(LG_DEBUG, "WROOT!");
    if (!wroot) {
        slog(LG_DEBUG, "Error parsing JSON data: %s\n", werror.text);
        free(chunk.memory);
        curl_global_cleanup();
        return NULL;
    }

    json_t *wresults = json_object_get(wroot, "currently");
    if (!wresults) {
        slog(LG_DEBUG, "Error retrieving 'currently' from JSON data.\n");
        json_decref(wroot);
        free(chunk.memory);
        curl_global_cleanup();
        return NULL;
    }

    const char *wtype = json_string_value(json_object_get(wresults, "summary"));
    if (wtype) {
        snprintf(out, sizeof(out), "%s ", wtype);
        strncat(output, out, sizeof(output) - strlen(output) - 1);
    }

    double ftemp = json_number_value(json_object_get(wresults, "temperature"));
    double ctemp = (ftemp - 32) * 5 / 9;

    char temp_buffer[50];
    format_temp("F/C", ftemp, ctemp, temp_buffer, sizeof(temp_buffer));
    snprintf(out, sizeof(out), "%s", temp_buffer);
    strncat(output, out, sizeof(output) - strlen(output) - 1);

    double aftemp = json_number_value(json_object_get(wresults, "apparentTemperature"));
    double actemp = (aftemp - 32) * 5 / 9;

    format_temp("F/C", aftemp, actemp, temp_buffer, sizeof(temp_buffer));
    snprintf(out, sizeof(out), " | \2Feels Like\2: %s", temp_buffer);
    strncat(output, out, sizeof(output) - strlen(output) - 1);

    double whumid = json_number_value(json_object_get(wresults, "humidity"));
    snprintf(out, sizeof(out), " | \2Humidity\2: %.0f%%", whumid * 100);
    strncat(output, out, sizeof(output) - strlen(output) - 1);

    double wwind = json_number_value(json_object_get(wresults, "windSpeed"));
    double wwind_dir = json_number_value(json_object_get(wresults, "windBearing"));
    double gwind = json_number_value(json_object_get(wresults, "windGust"));
    double wkwind = wwind * 1.60934;
    double gkwind = gwind * 1.60934;
    snprintf(out, sizeof(out), " | \2Wind\2: %.1fmph/%.1fkm/h %s \2Gust\2: %.1fmph/%.1fkm/h", wwind, wkwind, wind_direction((int)wwind_dir), gwind, gkwind);
    strncat(output, out, sizeof(output) - strlen(output) - 1);

    double wdew = json_number_value(json_object_get(wresults, "dewPoint"));
    snprintf(out, sizeof(out), " | \2Dew\2: %.0f°", wdew);
    strncat(output, out, sizeof(output) - strlen(output) - 1);

    double wuv = json_number_value(json_object_get(wresults, "uvIndex"));
    char* color;
    const char* risk = format_uv(wuv, &color);
    snprintf(out, sizeof(out), " | \2UV Index\2: %.1f \2Risk\2: %s%s\017", wuv, color, risk);
    strncat(output, out, sizeof(output) - strlen(output) - 1);

    struct tm *risetimeinfo;
    struct tm *settimeinfo;
    char rise_buffer[20];
    char set_buffer[20];
    setenv("TZ", "America/New_York", 1);
    tzset();

    json_t *daily = json_object_get(wroot, "daily");
    json_t *dailydate = json_object_get(daily, "data");
    json_t *today = json_array_get(dailydate, 0);

    int sunrise = json_integer_value(json_object_get(today, "sunriseTime"));
    int sunset = json_integer_value(json_object_get(today, "sunsetTime"));

    time_t sunset_time = sunset;
    settimeinfo = convert_to_eastern_time(sunset_time);
    strftime(set_buffer, sizeof(set_buffer), "%I:%M %p %Z", settimeinfo);

    time_t sunrise_time = sunrise;
    risetimeinfo = convert_to_eastern_time(sunrise_time);
    strftime(rise_buffer, sizeof(rise_buffer), "%I:%M %p %Z", risetimeinfo);
    snprintf(out, sizeof(out), " | \2Sunrise\2: %s \2Sunset\2: %s", rise_buffer, set_buffer);
    strncat(output, out, sizeof(output) - strlen(output) - 1);

    slog(LG_DEBUG, "MADE IT HERE!");
    size_t index;
    json_t *value;
    time_t current_time = time(NULL);
    struct tm *ctimeinfo;
    char cdate_buffer[11];
    char ncdate_buffer[11];
    ctimeinfo = convert_to_eastern_time(current_time);
    strftime(cdate_buffer, sizeof(cdate_buffer), "%Y-%m-%d", ctimeinfo);

    char foutput[FORECAST_SIZE] = "";
    char fout[250] = "";

    int element_count = 0;
    char low_temp_buffer[500];
    char high_temp_buffer[500];

    if (forecast == 1) {
        snprintf(fout, sizeof(fout), "\2%s\2 :: Forecast",location);
        strcat(foutput, fout);
    }
    json_array_foreach(dailydate, index, value) {
        if (forecast == 0 && element_count >= 3) {
            break;
        }

        int date = json_integer_value(json_object_get(value, "time"));
        const char *nwtype = json_string_value(json_object_get(value, "summary"));
        double nwtempH = json_number_value(json_object_get(value, "temperatureHigh"));
        double nwtempL = json_number_value(json_object_get(value, "temperatureLow"));
        double nctempH = (nwtempH - 32) * 5 / 9;
        double nctempL = (nwtempL - 32) * 5 / 9;
        time_t newdate = date;
        struct tm *newdateinfo = gmtime(&newdate);
        char date_buffer[20];
        strftime(ncdate_buffer, sizeof(ncdate_buffer), "%Y-%m-%d", newdateinfo);
        if (strcmp(cdate_buffer, ncdate_buffer) != 0) {
            if (forecast == 0) {
                strftime(date_buffer, sizeof(date_buffer), "%a", newdateinfo);
            } else {
                strftime(date_buffer, sizeof(date_buffer), "%A", newdateinfo);
            }
            format_temp("L", nwtempL, nctempL, low_temp_buffer, sizeof(low_temp_buffer));
            format_temp("H", nwtempH, nctempH, high_temp_buffer, sizeof(high_temp_buffer));
            snprintf(fout, sizeof(fout), " | \2%s\2: %s %s %s", date_buffer, nwtype, low_temp_buffer, high_temp_buffer);
            strncat(foutput, fout, sizeof(foutput) - strlen(foutput) - 1);
        }

        element_count++;
    }


    json_decref(wroot);
    free(chunk.memory);

    curl_global_cleanup();

    if (forecast == 1) {
       char *final_output = malloc(strlen(foutput) + 1);
       strcpy(final_output, foutput);
       return final_output;
       }
    slog(LG_DEBUG, "AT FINAL!");
    slog(LG_DEBUG, output);
    slog(LG_DEBUG, foutput);
    // Combine output and foutput into one string
    char *final_output = malloc(strlen(output) + strlen(foutput) + 1);
    if (final_output) {
        strcpy(final_output, output);
        strcat(final_output, foutput);
    }

    return final_output;  // Return the combined string
    slog(LG_DEBUG, final_output);
}



// Hook function to handle channel messages
static void on_channel_message(hook_cmessage_data_t *data) {
    if (data->msg && (strncmp(data->msg, "!weather", 8) == 0 || strncmp(data->msg, "!w", 2) == 0)) {

       const char *input = data->msg;
       char command[256];
       char templocation[256];
 
       split_command_location(input, command, templocation);
       size_t length = strlen(templocation);
    if (length == 0) {
        strcpy(templocation, "False");
    }

    char *weather_data;

    if (strcmp(templocation, "False") == 0) {
    	if(data->u->myuser == NULL) {
        	msg("Weather", data->c->name, "No location was requested or use SETWEATHER to set default location.");
	        return;
        }
        const char *location;
        const char *latlong;
        metadata_t *md = metadata_find(data->u->myuser, "private:weather:location");
        if (md != NULL) {
            location = md->value;
            metadata_t *md = metadata_find(data->u->myuser, "private:weather:latlong");
            latlong = md->value;
            weather_data = fetch_weather_data(location, latlong, 0);
            const char *colors = metadata_find(data->u->myuser, "private:weather:colors")->value;
            if (colors && !strcasecmp(colors, "OFF")) {
                  remove_colors(weather_data);
            }
            msg("Weather", data->c->name, weather_data);
            return;
        } else {
            msg("Weather", data->c->name, "No location was requested or use SETWEATHER to set default location.");
            return;
        }
    }
    char location[256];
    snprintf(location, sizeof(location), "%s", templocation);
    replace_spaces_with_underscores(location);
    OpenCage result = fetch_geocode_data(location);
    if (result.error_code == 0) {
        weather_data = fetch_weather_data(result.location, result.latlong, 0);
        const char *colors = metadata_find(data->u->myuser, "private:weather:colors")->value;
        if (colors && !strcasecmp(colors, "OFF")) {
                remove_colors(weather_data);
        }
        msg("Weather", data->c->name, weather_data);
    } else {
        msg("Weather", data->c->name, "Error: %s", result.location);
    }
    }
    if (data->msg && (strncmp(data->msg, "!forecast", 8) == 0 || strncmp(data->msg, "!f", 2) == 0)) {

       const char *input = data->msg;
       char command[256];
       char templocation[256];

       split_command_location(input, command, templocation);
       size_t length = strlen(templocation);
    if (length == 0) {
        strcpy(templocation, "False");
    }

    char *weather_data;

    if (strcmp(templocation, "False") == 0) {
        if(data->u->myuser == NULL) {
                msg("Weather", data->c->name, "No location was requested or use SETWEATHER to set default location.");
                return;
        }
        const char *location;
        const char *latlong;
        metadata_t *md = metadata_find(data->u->myuser, "private:weather:location");
        if (md != NULL) {
            location = md->value;
            metadata_t *md = metadata_find(data->u->myuser, "private:weather:latlong");
            latlong = md->value;
            weather_data = fetch_weather_data(location, latlong, 1);
            const char *colors = metadata_find(data->u->myuser, "private:weather:colors")->value;
            if (colors && !strcasecmp(colors, "OFF")) {
                  remove_colors(weather_data);
            }
            msg("Weather", data->c->name, weather_data);
            return;
        } else {
            msg("Weather", data->c->name, "No location was requested or use SETWEATHER to set default location.");
            return;
        }
    }
    char location[256];
    snprintf(location, sizeof(location), "%s", templocation);
    replace_spaces_with_underscores(location);
    OpenCage result = fetch_geocode_data(location);
    if (result.error_code == 0) {
        weather_data = fetch_weather_data(result.location, result.latlong, 1);
        const char *colors = metadata_find(data->u->myuser, "private:weather:colors")->value;
        if (colors && !strcasecmp(colors, "OFF")) {
                remove_colors(weather_data);
        }
        msg("Weather", data->c->name, weather_data);
    } else {
        msg("Weather", data->c->name, "Error: %s", result.location);
    }
    }


}


// Function to join a channel and update the channel table
static void ws_cmd_join(sourceinfo_t *si, int parc, char *parv[]) {
        if (parc < 1) {
                command_fail(si, fault_needmoreparams, "Usage: JOIN <#channel>");
                return;
        }

        const char *channel = parv[0];

        // Check if user is identified and has access via ChanServ
        mychan_t *mc = mychan_find(channel);
        if (!mc || !chanacs_user_has_flag(mc, si->su, CA_INVITE)) {
                command_fail(si, fault_noprivs, "You do not have access for %s to join %s.", si->service->nick, channel);
                return;
        }

        // Check if the bot is already in the channel
        channel_info_t *ci = mowgli_patricia_retrieve(channel_table, channel);
        if (!ci) {
                ci = malloc(sizeof(channel_info_t));
                ci->channel = strdup(channel);
                ci->requester = strdup(si->su->nick);

                mowgli_patricia_add(channel_table, ci->channel, ci);

                // Join the channel
                command_success_nodata(si, "Joining %s...", channel);
                join(channel, si->service->nick);
        } else {
                // Check if the bot is in the channel
                if (!channel_find(channel)) {
                        command_success_nodata(si, "Joining %s...", channel);
                        join(channel, si->service->nick);
                }
        }
}


// Function to save the channel table to a file
void save_channel_table(const char *filename) {
    FILE *file = fopen(filename, "wb");
    if (file == NULL) {
        perror("Failed to open file for writing");
        return;
    }

    mowgli_patricia_iteration_state_t state;
    channel_info_t *ci;
    
    MOWGLI_PATRICIA_FOREACH(ci, &state, channel_table) {
        size_t channel_len = strlen(ci->channel) + 1;
        size_t requester_len = strlen(ci->requester) + 1;

        fwrite(&channel_len, sizeof(size_t), 1, file);
        fwrite(ci->channel, sizeof(char), channel_len, file);

        fwrite(&requester_len, sizeof(size_t), 1, file);
        fwrite(ci->requester, sizeof(char), requester_len, file);
    }

    fclose(file);
}

// Function to load the channel table from a file
void load_channel_table(const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        save_channel_table("channel_table.db");
    }

    while (!feof(file)) {
        size_t channel_len;
        size_t requester_len;

        if (fread(&channel_len, sizeof(size_t), 1, file) != 1) break;
        char *channel = malloc(channel_len);
        fread(channel, sizeof(char), channel_len, file);

        if (fread(&requester_len, sizeof(size_t), 1, file) != 1) break;
        char *requester = malloc(requester_len);
        fread(requester, sizeof(char), requester_len, file);

        channel_info_t *ci = malloc(sizeof(channel_info_t));
        ci->channel = channel;
        ci->requester = requester;

        mowgli_patricia_add(channel_table, ci->channel, ci);
    }

    fclose(file);
}


// Function to cycle through channels and join if not already in
static void ws_cmd_cycle(sourceinfo_t *si, int parc, char *parv[]) {
        int channels_joined = 0;
        mowgli_patricia_iteration_state_t state;
        channel_info_t *ci;

        MOWGLI_PATRICIA_FOREACH(ci, &state, channel_table) {
                        join(ci->channel, si->service->nick);
                        channels_joined++;
        }

        command_success_nodata(si, "Cycle complete. Channels joined: %d", channels_joined);
        save_channel_table("channel_table.db");

}

void _modinit(module_t *m)
{
    weather = service_add("weather", NULL);
    set_limit.hitvalue = 10;
    service_bind_command(weather, &ws_help);
    service_bind_command(weather, &ws_weather);
    service_bind_command(weather, &ws_w);
    service_bind_command(weather, &ws_forecast);
    service_bind_command(weather, &ws_f);
    service_bind_command(weather, &ws_setweather);
    service_bind_command(weather, &ws_setcolors);
    service_bind_command(weather, &ws_setgreet);
    service_bind_command(weather, &ws_setratelimit);
    service_bind_command(weather, &ws_info);
    service_bind_command(weather, &ws_join);
    service_bind_command(weather, &ws_cycle);

    hook_add_event("channel_message");
    hook_add_channel_message(on_channel_message);

    hook_add_event("user_identify");
    hook_add_user_identify(on_user_identify);

    init_rate_limit();
    init_channel_table();

    load_channel_table("channel_table.db");
   // ws_cmd_cycle(NULL, 0, NULL);
}

void _moddeinit(module_unload_intent_t intent)
{
    service_unbind_command(weather, &ws_help);
    service_unbind_command(weather, &ws_weather);
    service_unbind_command(weather, &ws_w);
    service_unbind_command(weather, &ws_forecast);
    service_unbind_command(weather, &ws_f);
    service_unbind_command(weather, &ws_setweather);
    service_unbind_command(weather, &ws_setgreet);
    service_unbind_command(weather, &ws_setratelimit);
    service_unbind_command(weather, &ws_setcolors);
    service_unbind_command(weather, &ws_info);
    service_unbind_command(weather, &ws_join);
    service_unbind_command(weather, &ws_cycle);
    hook_del_channel_message(on_channel_message);
    hook_del_user_identify(on_user_identify);
    mowgli_patricia_destroy(rate_limit_table, rate_limit_free, NULL);
    mowgli_patricia_destroy(channel_table, channel_info_free, NULL);
    save_channel_table("channel_table.db");
    service_delete(weather);

}
