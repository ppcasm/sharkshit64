#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi_types.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "esp_hid_host.h"
#include "http_ui.h"
#include "modem.h"

// Used in our gamegenie proxy
#define INITIAL_BUFFER_SIZE 4096
#define MAX_DOWNLOAD_SIZE (256 * 1024)

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

static const char *HTTP_UI_TAG = "HTTP_UI";

// Used in our POST handlers when extracting post data
typedef struct {
    char key[32];
    char value[128];
} FormField;

// parse_post_data
// Used to parse post data key-value pairs
void parse_post_data(char *data, FormField *fields, int *field_count) {
    *field_count = 0;

    char *pair = strtok(data, "&");
    while (pair != NULL && *field_count < 10) {
        char *eq = strchr(pair, '=');
        if (eq != NULL) {
            *eq = '\0';
            char *key = pair;
            char *value = eq + 1;

            for (char *p = value; *p; p++) {
                if (*p == '+') *p = ' ';
            }

            strncpy(fields[*field_count].key, key, sizeof(fields[*field_count].key) - 1);
            strncpy(fields[*field_count].value, value, sizeof(fields[*field_count].value) - 1);
            (*field_count)++;
        }
        pair = strtok(NULL, "&");
    }
}

// get_post_value
// Used to get post data value from key
const char *get_post_value(const char *key, FormField *fields, int field_count) {
    for (int i = 0; i < field_count; i++) {
        if (strcmp(fields[i].key, key) == 0) {
            return fields[i].value;
        }
    }
    return NULL;
}

// sharkwire_encode
// When sharkwire goes to do any settings changes and/or activation, it uses a 
// special tagging system where certain mega tags have their content "encoded"
// before it'll attempt to set values. This encoding is based off of the remote IP
// address, which would be known server side and passed in as a response with proper
// formatting. This is the encode portion of that action.  
static char *sharkwire_encode(const char *tag, const char *ip) {
    // We used the power of jhynjhiruu to figure this shit out :p 
    size_t tag_len = strlen(tag);
    size_t ip_len = strlen(ip);
    size_t out_len = tag_len * 2;

    // Allocate the output string +1 for null terminator
    char *outbuf = malloc(out_len + 1);
    if (!outbuf) return NULL;

    for (size_t tag_index = 0, ip_index = 0; tag_index < tag_len; tag_index++, ip_index++) {
        unsigned char encoded_value = (tag[tag_index] + ip[ip_index % ip_len] - 1) % 255;
        sprintf(&outbuf[tag_index * 2], "%02X", encoded_value);
    }

    outbuf[out_len] = '\0';
    return outbuf;
}

// save_sta_credentials
// Saves the credentials to NVS for the ESP32 <-> router connection
void save_sta_credentials(const char *ssid, const char *pass) {
    nvs_handle_t handle;
    nvs_open("wifi", NVS_READWRITE, &handle);
    nvs_set_str(handle, "ssid", ssid);
    nvs_set_str(handle, "pass", pass);
    nvs_commit(handle);
    nvs_close(handle);
}

// load_sta_credentials
// Loads the credentials from NVS for the ESP32 <-> router connection
bool load_sta_credentials(wifi_config_t *sta_config) {
    nvs_handle_t handle;
    size_t size;
    if (nvs_open("wifi", NVS_READONLY, &handle) != ESP_OK) return false;
    size = sizeof(sta_config->sta.ssid);
    if (nvs_get_str(handle, "ssid", (char*)sta_config->sta.ssid, &size) != ESP_OK) {
        nvs_close(handle); return false;
    }
    size = sizeof(sta_config->sta.password);
    if (nvs_get_str(handle, "pass", (char*)sta_config->sta.password, &size) != ESP_OK) {
        nvs_close(handle); return false;
    }
    nvs_close(handle);
    return true;
}

// config_post_handler
// This handles the POST action of the config WiFi credentials setup page that displays at 192.168.4.1
// when you connect to the ESP32 SSID while its AP is running
esp_err_t config_post_handler(httpd_req_t *req) {

    if ((req->method == HTTP_POST) && strcmp(req->uri, "/unbond_ble_device") == 0) {
        unbond_ble_device();
        httpd_resp_send(req,
        "<html><body style=\"background-color:black;\">"
        "<center><strong><h3><p style=\"color:red;\">"
        "Power cycle the Nintendo64 to finalize BLE device unbonding"
        "</p></h3></strong></center>"
        "</body></html>",
        HTTPD_RESP_USE_STRLEN);
   } else {
        char buf[128];
        int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (len <= 0) return ESP_FAIL;
        buf[len] = 0;

        char ssid[32] = {0}, pass[64] = {0};
        sscanf(buf, "ssid=%31[^&]&pass=%63s", ssid, pass);
        save_sta_credentials(ssid, pass);

        httpd_resp_send(req,
        "<html><body style=\"background-color:black; color:white;\">"
        "<center><strong><h3><p style=\"color:red;\">"
        "Power cycle the Nintendo64 to finalize WiFi configuration"
        "</p></h3></strong></center>"
        "</body></html>",
        HTTPD_RESP_USE_STRLEN);
    }
    //vTaskDelay(pdMS_TO_TICKS(1000));
    //esp_restart();
    return ESP_OK;
}

// ble_status_get_handler
// This is used to get the ble status so that it can be updated at an interval without having to
// refresh the entire config page 
static esp_err_t ble_status_get_handler(httpd_req_t *req) {

    char *ble_section = malloc(4096);
    if (!ble_section) {
        free(ble_section);
        ESP_LOGE("CONFIG", "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }

    char bda_str[64] = "N/A";
    esp_bd_addr_t bda;

    if (get_bonded_device_address(bda)) {
        snprintf(bda_str, sizeof(bda_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    }

    bool connected = is_ble_connected();
    int battery = get_ble_battery_level();
    char battery_str[16] = "N/A";
    if (connected && battery >= 0) {
        if (battery > 100) battery = 100;
        snprintf(battery_str, sizeof(battery_str), "%d%%", battery);
    }

    char restart_n64_msg[128] = "";
    if (strcmp(bda_str, "N/A") == 0 && connected) {
        snprintf(restart_n64_msg, sizeof(restart_n64_msg),
            "<strong><p style=\"color:red;\">Power cycle the Nintendo64 to finalize BLE bonding</p></strong>");
    }

    snprintf(ble_section, 512,
        "<center><h3>BLE Status</h3></center>"
        "<p>Bonded Device: %s</p>"
        "<p>Connected: %s</p>"
        "<p>Manufacturer: %s</p>"
        "<p>Battery: %s</p>"
        "%s"
        "<form method=\"POST\" action=\"/unbond_ble_device\">"
        "<input type=\"submit\" value=\"Unbond BLE Device\">"
        "</form>",
        bda_str,
        connected ? "Yes" : "No",
        get_ble_manufacturer(),
        battery_str,
        restart_n64_msg
    );

    httpd_resp_send(req, ble_section, HTTPD_RESP_USE_STRLEN);
    free(ble_section);

    return ESP_OK;
}

// add_line_breaks
// This was a quick hack to do some parsing on the <pre> tag data from
// our gamegenie proxy, because I didn't like the way it rendered preformatted
// text, so I did away with that and just forced line breaks because it looked
// better when using non pre tag font sizing
char *add_line_breaks(const char *input, size_t *out_len) {
    size_t len = strlen(input);
    char *output = malloc(len * 6 + 1);
    if (!output) return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        // Skip opening <pre> tags
        if (strncasecmp(&input[i], "<pre", 4) == 0) {
            i += 4;
            while (i < len && input[i] != '>') i++;
            if (i < len && input[i] == '>') i++;
            continue;
        }

        // Skip closing </pre> tag
        if (strncasecmp(&input[i], "</pre>", 6) == 0) {
            i += 6;
            continue;
        }

        // Copy character and insert <br> after newline
        char c = input[i++];
        output[j++] = c;
        if (c == '\n') {
            memcpy(&output[j], "<br>", 4);
            j += 4;
        }
    }

    output[j] = '\0';
    if (out_len) *out_len = j;
    return output;
}

// extract_section
// This is used as a quick hack to extract sections by markers, used mostly to piece together the sections for
// the gamegenie.com proxy
static char *extract_section(const char *start_marker, const char *end_marker, const char *source, size_t *out_len) {
    char *start = strstr(source, start_marker);
    if (!start) return NULL;
    start += strlen(start_marker);

    char *end = strstr(start, end_marker);
    if (!end || end <= start) return NULL;

    *out_len = end - start;
    char *result = malloc(*out_len + 1);
    if (!result) return NULL;

    strncpy(result, start, *out_len);
    result[*out_len] = '\0';
    return result;
}

// gamegenie_proxy_handler
// Basically this handles access to gamegenie.com which we use for the 
// "Gamerz" Sharkwire home page link, and this works by going directly to the gameshark
// section of the gamegenie.com website via the Gamerz link, then that causes our custom DNS
// server to point to ESP32, which then causes it to go here via a registered handler for the
// specified path. When that happens, ESP32 will then reach out to the HTTPS version of the site
// and extract the needed HTML sections in order to build a (hopefully) compatible stripped down
// interface that renders in the content section of our sharkwire online home page
esp_err_t gamegenie_proxy_handler(httpd_req_t *req) {
    const char *base_path = "/cheats/gameshark/n64/";
    const char *request_path = req->uri;

    if (strncmp(request_path, base_path, strlen(base_path)) != 0) {
        httpd_resp_send_err(req, 400, "invalid path");
        return ESP_FAIL;
    }

    char *full_url = malloc(1024);
    if (!full_url) {
        httpd_resp_send_err(req, 500, "malloc failed");
        return ESP_ERR_NO_MEM;
    }

    snprintf(full_url, 1024, "https://gamegenie.com%s", request_path);

    esp_http_client_config_t config = {
        .url = full_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(full_url);
        httpd_resp_send_err(req, 502, "client init failed");
        return ESP_FAIL;
    }

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        free(full_url);
        httpd_resp_send_err(req, 502, "open failed");
        return ESP_FAIL;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        esp_http_client_cleanup(client);
        free(full_url);
        httpd_resp_send_err(req, 502, "failed to fetch headers");
        return ESP_FAIL;
    }

    char *body = malloc(INITIAL_BUFFER_SIZE);
    if (!body) {
        esp_http_client_cleanup(client);
        free(full_url);
        httpd_resp_send_err(req, 500, "malloc failed");
        return ESP_ERR_NO_MEM;
    }

    size_t capacity = INITIAL_BUFFER_SIZE;
    size_t total = 0;

    while (1) {
        if (total + 1024 > capacity) {
            if (capacity >= MAX_DOWNLOAD_SIZE) {
                free(body);
                free(full_url);
                esp_http_client_cleanup(client);
                httpd_resp_send_err(req, 500, "body too large");
                return ESP_FAIL;
            }
            capacity *= 2;
            char *new_body = realloc(body, capacity);
            if (!new_body) {
                free(body);
                free(full_url);
                esp_http_client_cleanup(client);
                httpd_resp_send_err(req, 500, "realloc failed");
                return ESP_ERR_NO_MEM;
            }
            body = new_body;
        }

        int r = esp_http_client_read(client, body + total, capacity - total);
        if (r < 0) {
            free(body);
            free(full_url);
            esp_http_client_cleanup(client);
            httpd_resp_send_err(req, 502, "read failed");
            return ESP_FAIL;
        } else if (r == 0) {
            break;
        }
        total += r;
    }

    body[total] = '\0';
    esp_http_client_cleanup(client);
    free(full_url);

    const char *start_marker = strstr(request_path, "index.html") ? "<!-- NAME -->" : "<!-- GAME CHEATS -->";
    const char *end_marker = "<!-- DNET LINKS & BANNER -->";

    size_t content_len = 0;
    char *raw_content = extract_section(start_marker, end_marker, body, &content_len);
    if (!raw_content) { free(body); httpd_resp_send_err(req, 500, "no content"); return ESP_FAIL; }

    char *content = raw_content;
    if (!strstr(request_path, "index.html")) {
        size_t new_len = 0;
        content = add_line_breaks(raw_content, &new_len);
        free(raw_content);
        if (!content) { free(body); httpd_resp_send_err(req, 500, "br insert failed"); return ESP_FAIL; }
        content_len = new_len;
    }

    size_t title_len = 0;
    char *title = extract_section("<!-- NAME -->", "<!-- /NAME -->", body, &title_len);

    if (!content || !title) {
        free(body);
        if (content) free(content);
        if (title) free(title);
        httpd_resp_send_err(req, 500, "required sections not found");
        return ESP_FAIL;
    }

   const char *html_template =
    "<html><head><title>%s</title></head>"
    "<center><font size=\"1\">Powered by gamegenie.com</font></center>"
    // SharkWire color scheme: "<body bgcolor=\"#000099\" text=\"#E0E040\" link=\"#FFCC00\" vlink=\"#FFCC00\" alink=\"#FFCC00\">"
    "<body bgcolor=\"#FFFFFF\" text=\"#000000\" link=\"#0000FF\" vlink=\"#800080\" alink=\"#FF0000\">"
    "<font size=\"1\">%s</font>"
    "</body></html>";

    size_t html_len = strlen(html_template) + title_len + content_len + 1;

    char *full_html = malloc(html_len);
    if (!full_html) {
        free(body);
        free(content);
        free(title);
        httpd_resp_send_err(req, 500, "malloc failed");
        return ESP_ERR_NO_MEM;
    }

    snprintf(full_html, html_len, html_template, title, content);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, full_html, strlen(full_html));

    free(full_html);
    free(body);
    free(content);
    free(title);
    return ESP_OK;
}

// config_get_handler
// This handles the GET request of the config WiFi credentials setup page that displays at 192.168.4.1
// when you connect to the ESP32 SSID while its AP is running, and it also shows the current bonded
// BLE device information (we only support 1 for now)
esp_err_t config_get_handler(httpd_req_t *req) {

   wifi_config_t sta_config = {0};

   char *html = malloc(4096);
   if (!html) {
       free(html);
       ESP_LOGE("CONFIG", "Memory allocation failed");
       return ESP_ERR_NO_MEM;
   }
    
   bool has_sta = load_sta_credentials(&sta_config);

   // Ensure null termination on NVS string
   sta_config.sta.ssid[sizeof(sta_config.sta.ssid) - 1] = 0;
   sta_config.sta.password[sizeof(sta_config.sta.password) - 1] = 0;

   char *ble_section = malloc(512);
   if (!ble_section) {
       free(html);
       ESP_LOGE("CONFIG", "Memory allocation failed");
       return ESP_ERR_NO_MEM;
   }

   char bda_str[64] = "N/A";
   esp_bd_addr_t bda;

   if (get_bonded_device_address(bda)) {
       snprintf(bda_str, sizeof(bda_str), "%02X:%02X:%02X:%02X:%02X:%02X",
           bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
   }

   bool connected = is_ble_connected();
   int battery = get_ble_battery_level();
   char battery_str[16] = "N/A";
   if (connected && battery >= 0) {
       if (battery > 100) battery = 100;
       snprintf(battery_str, sizeof(battery_str), "%d%%", battery);
   }

   char restart_n64_msg[128] = "";
   if (strcmp(bda_str, "N/A") == 0 && connected) {
       snprintf(restart_n64_msg, sizeof(restart_n64_msg),
           "<strong><p style=\"color:red;\">Power cycle the Nintendo64 to finalize BLE bonding</p></strong>");
   }

   snprintf(ble_section, 512,
       "<center><h3>BLE Status</h3></center>"
       "<p>Bonded Device: %s</p>"
       "<p>Connected: %s</p>"
       "<p>Manufacturer: %s</p>"
       "<p>Battery: %s</p>"
       "%s"
       "<form method=\"POST\" action=\"/unbond_ble_device\">"
       "<input type=\"submit\" value=\"Unbond BLE Device\">"
       "</form>",
       bda_str,
       connected ? "Yes" : "No",
       get_ble_manufacturer(),
       battery_str,
       restart_n64_msg
   );

   int html_len;
   if (has_sta) {
       html_len = snprintf(html, 4096,
           "<html><head><style>"
           "body { background:white; color:black; font-family:sans-serif; display:flex; justify-content:center; align-items:center; height:100vh; }"
           ".container { width: 400px; padding:20px; border:1px solid #ccc; border-radius:8px; box-shadow:2px 2px 12px rgba(0,0,0,0.1); }"
           "input[type=text], input[type=password] { width:100%%; padding:8px; margin:6px 0; box-sizing:border-box; }"
           "input[type=submit] { width:100%%; padding:10px; background:#007acc; color:white; border:none; border-radius:4px; cursor:pointer; }"
           "input[type=submit]:hover { background:#005f99; }"
           "</style></head><body><div class='container'>"
           "<center><h1><strong>SharkShit64</strong></h1></center>"
           "<center><h3>Wi-Fi Settings</h3></center>"
           "<form method='POST'>"
           "SSID:<input type='text' name='ssid' value='%s'><br>"
           "Password:<input type='password' name='pass' value='%s'><br>"
           "<input type='submit' value='Save WiFi Credentials'></form>"
           "<div id='ble-status'>%s</div>"
           "</div>"
           "<script>"
           "function updateBLE() {"
           "  var xhr = new XMLHttpRequest();"
           "  xhr.onreadystatechange = function() {"
           "    if (xhr.readyState == 4 && xhr.status == 200) {"
           "      document.getElementById('ble-status').innerHTML = xhr.responseText;"
           "    }"
           "  };"
           "  xhr.open('GET', '/ble_status', true);"
           "  xhr.send();"
           "}"
           "setInterval(updateBLE, 5000);"
           "</script>"
           "</body></html>",
           (char *)sta_config.sta.ssid,
           (char *)sta_config.sta.password,
           ble_section
       );
   } else {
       html_len = snprintf(html, 4096,
           "<html><head><style>"
           "body { background:white; color:black; font-family:sans-serif; display:flex; justify-content:center; align-items:center; height:100vh; }"
           ".container { width: 400px; padding:20px; border:1px solid #ccc; border-radius:8px; box-shadow:2px 2px 12px rgba(0,0,0,0.1); }"
           "input[type=text], input[type=password] { width:100%%; padding:8px; margin:6px 0; box-sizing:border-box; }"
           "input[type=submit] { width:100%%; padding:10px; background:#007acc; color:white; border:none; border-radius:4px; cursor:pointer; }"
           "input[type=submit]:hover { background:#005f99; }"
           "</style></head><body><div class='container'>"
           "<center><h1><strong>SharkShit64</strong></h1></center>"
           "<center><h3>Wi-Fi Settings</h3></center>"
           "<form method='POST'>"
           "SSID:<input type='text' name='ssid'><br>"
           "Password:<input type='password' name='pass'><br>"
           "<input type='submit' value='Save Wifi Credentials'></form>"
           "<div id='ble-status'>%s</div>"
           "</div>"
           "<script>"
           "function updateBLE() {"
           "  var xhr = new XMLHttpRequest();"
           "  xhr.onreadystatechange = function() {"
           "    if (xhr.readyState == 4 && xhr.status == 200) {"
           "      document.getElementById('ble-status').innerHTML = xhr.responseText;"
           "    }"
           "  };"
           "  xhr.open('GET', '/ble_status', true);"
           "  xhr.send();"
           "}"
           "setInterval(updateBLE, 5000);"
           "</script>"
           "</body></html>",
           ble_section
       );
   }

   if (html_len >= 4096) {
       ESP_LOGW("CONFIG", "HTML output truncated (%d bytes)", html_len);
   }

   httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);

   free(html);
   free(ble_section);

   return ESP_OK;
}

// home_get_handler
// This handles the GET request from whenever Sharkwire attempts to land on the
// sharkwireonline.com homepage
esp_err_t home_get_handler(httpd_req_t *req) {

    char *html = malloc(4096);
    if (!html) {
        ESP_LOGE("CONFIG", "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }

    snprintf(html, 4096,
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<title>SharkWire Online</title>"
        "</head>"
        "<frameset cols=\"25%%,75%%\">"
        "<frame src=\"menu.htm\" name=\"menu\">"
        "<frame src=\"content.htm\" name=\"content\">"
        "</frameset>"
        "</html>"
    );

    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);

    free(html);
    return ESP_OK;
}

esp_err_t menu_get_handler(httpd_req_t *req) {
    const char *html =
        "<html><head><title>SharkWire Online</title></head>"
        "<body bgcolor=\"#000099\" text=\"#E0E040\" link=\"#FFCC00\" vlink=\"#FFCC00\" alink=\"#FFCC00\">"
        "<body background=\"file:///c:/dm/shrkimg/all_w_xx_circles.gif\">"
        "<img src=\"file:///c:/dm/shrkimg/all_l_xx_logo.gif\" border=\"0\" vspace=\"0\" hspace=\"0\">"

        "<a href=\"http://gamegenie.com/cheats/gameshark/n64/index.html\" target=\"content\">"
        "<img src=\"file:///c:/dm/shrkimg/gmr_n_xx.gif\" border=\"0\" vspace=\"2\" hspace=\"2\"></a>"

        "<a href=\"http://68k.news\" target=\"content\">"
        "<img src=\"file:///c:/dm/shrkimg/klt_n_xx.gif\" border=\"0\" vspace=\"2\" hspace=\"2\"></a>"

        "</body></html>";

    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t content_get_handler(httpd_req_t *req) {
    const char *html =
        "<html><title>SharkWire Online</title>"
        "<body bgcolor=\"#000099\" text=\"#E0E040\" link=\"#FFCC00\" vlink=\"#FFCC00\" alink=\"#FFCC00\">"
        "<h1>Welcome to Sharkwire Online</h1>"
        "</body></html>";

    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// activation_handler
// This handles all activation and settings changes for Sharkwire
//
// It processes as a single handler but really works in 2 parts ( act1 and act2 )
// where act1 is for setting up the ISP connection ( username, password, access number,
// and dialing prefix ) and returns some specialized meta content by encoding them with
// the sharkwire_encode function
//
// act2 is for setting up Sharkwire username/email accounts, like when going to "Add new user"
// from the sharkwire menu. This works in much the same way but has a few more tags it sets in
// order to do setup
//
// There are a number of tags which can be set during activation, and they require that certain
// tags be set, as well as a "SHARKWIRE_MAGIC" meta tag that must exist. Some examples of tags
// that can be set are as listed below, with the first 3 seemingly being needed in order to
// finalize the tag settings in persistent storage
//
// Activation example tags:
//    <meta name="TARGETRES" content="640x240">
//    <meta name="WINDOWTYPE" content="POPUP">
//    <meta name="SHARKWIRE_MAGIC" content="D64A2756_SW_FE62">
//    <meta name="SHARKWIRE_ACCOUNTID" content="acti1">
//    <meta name="SHARKWIRE_ACCOUNTPWD" content="acti1">
//    <meta name="SHARKWIRE_PACCESS" content="08450804000">
//    <meta name="SHARKWIRE_SACCESS" content="08450804000">
//    <meta name="SHARKWIRE_SLOCATION" content="New York">
//    <meta name="SHARKWIRE_PLOCATION" content="New York">
//    <meta name="SHARKWIRE_PDNS" content="100.200.100.200">
//    <meta name="SHARKWIRE_SDNS" content="200.100.200.100">
//    <meta name="SHARKWIRE_EMAILIP" content="1.2.3.4">
//    <meta name="SHARKWIRE_EMAIL_GETCGI" content="/getcgi">
//    <meta name="SHARKWIRE_PCONTENTCGI" content="/contentcgi">
//    <meta name="SHARKWIRE_PEMAILCGI" content="/sendcgi">
//    <meta name="SHARKWIRE_PCONTENTIP" content="10.11.12.13">
//    <meta name="SHARKWIRE_SCONTENTIP" content="20.21.22.23">
//    <meta name="SHARKWIRE_CANGOTO" content="NO">
//    <meta name="SHARKWIRE_PROXY" content="100.101.102.103:8080">
//    <meta name="SHARKWIRE_LASTTAG" content="">
//
// All of the tags after "SHARKWIRE_MAGIC" must be encoded and "SHARKWIRE_LASTTAG"
// must always exist as the last tag in the set

esp_err_t activation_handler(httpd_req_t *req) {
    ESP_LOGI(HTTP_UI_TAG, "Request: %s %s",
             (req->method == HTTP_GET) ? "GET" :
             (req->method == HTTP_POST) ? "POST" :
             (req->method == HTTP_PUT) ? "PUT" :
             (req->method == HTTP_DELETE) ? "DELETE" : "OTHER",
             req->uri);

    // Only allocate if POST
    char *post_buf = NULL;
    FormField *fields = NULL;
    int field_count = 0;

    if (req->method == HTTP_POST) {
       post_buf = malloc(512);
       fields = malloc(sizeof(FormField) * 10);

       if (!post_buf || !fields) {
           ESP_LOGE(HTTP_UI_TAG, "Out of memory allocating buffers");
           if (post_buf) free(post_buf);
           if (fields) free(fields);
           return ESP_FAIL;
       }

       memset(post_buf, 0, 512);

       int remaining = req->content_len;
       int offset = 0;
       while (remaining > 0) {
           int received = httpd_req_recv(req, post_buf + offset, MIN(remaining, 512 - offset - 1));
           if (received <= 0) {
               if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
               free(post_buf);
               free(fields);
               return ESP_FAIL;
           }
           offset += received;
           remaining -= received;
       }

       ESP_LOGI(HTTP_UI_TAG, "POST raw: %s", post_buf);

       parse_post_data(post_buf, fields, &field_count);

   }

   if ((req->method == HTTP_POST || req->method == HTTP_GET) && strcmp(req->uri, "/cgi-bin/netshark/act_1") == 0) {

   const char *ip = ip4addr_ntoa(netif_ip4_gw(&ppp_netif));
   char *new_user_ok_encoded = sharkwire_encode("NEW_USER_OK", ip);

   if (!new_user_ok_encoded) {
       httpd_resp_send(req, "Error encoding tag", HTTPD_RESP_USE_STRLEN);
       return ESP_FAIL;
   }

   char *cangoto_yes_encoded = sharkwire_encode("YES", ip);

   if (!cangoto_yes_encoded) {
       httpd_resp_send(req, "Error encoding tag", HTTPD_RESP_USE_STRLEN);
       return ESP_FAIL;
   }

   char *resp = NULL;
   asprintf(&resp,
       "<HTML><head>"
       "<meta height=150 width=540 top=150 left=130 popup=1>"
       "<meta name=\"TARGETRES\" content=\"640x240\">"
       "<meta name=\"WINDOWTYPE\" content=\"POPUP\">"
       "<meta name=\"SHARKWIRE_MAGIC\" content=\"D64A2756_SW_FE62\">"
       "<meta name=\"SHARKWIRE_CANGOTO\" content=\"%s\">"
       "<meta name=\"SHARKWIRE_GENERAL_DISCONNECT\" content=\"%s\">"
       "<meta name=\"SHARKWIRE_LASTTAG\" content=\"\">"
       "<title>Sharkwire Activation</title></head>"
       "<body><h1>Sharkwire Activation</h1></body></HTML>", cangoto_yes_encoded, new_user_ok_encoded);

   if (!resp) {
       httpd_resp_send(req, "Error building HTML", HTTPD_RESP_USE_STRLEN);
       return ESP_FAIL;
   }

   httpd_resp_set_type(req, "text/html");
   httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
   ESP_LOGI(HTTP_UI_TAG, "Served ACT_1 page with encoded disconnect: %s", new_user_ok_encoded);

   free(new_user_ok_encoded);
   free(resp);

   }

   else if ((req->method == HTTP_POST || req->method == HTTP_GET) && strcmp(req->uri, "/cgi-bin/netshark/act_2") == 0) {

             const char *resp =
                 "<HTML><head>"
                 "<meta height=70 width=560 top=0 left=0 popup=1>"
                 "<meta name=\"TARGETRES\" content=\"640x240\">"
                 "<meta name=\"SHARKWIRE_MAGIC\" content=\"D64A2756_SW_FE62\">"
                 "<meta name=\"SHARKWIRE_LASTTAG\" content=\"\">"
                 "<title>Sharkwire new user activation</title></head>"
                 "<body bgcolor=\"#000099\" text=\"#FFFFCC\" link=\"#CCFFFF\">"
                 "<CENTER><IMG SRC=\"file:///c:/dm/html/interface_3/browser/images_03/sharkwire.gif\" ALT=\"Interact\"></CENTER>"
                 "<FORM name=\"nameandpassword\" METHOD=POST ACTION=\"/cgi-bin/netshark/newuserform\">"
                 "<center><table border=1>"
                 "<tr><td align=center>Enter Username:</td><td><input type=text name=\"t_name1\" size=16 maxlength=16></td></tr>"
                 "<tr><td align=center>Confirm Username:</td><td><input type=text name=\"t_name2\" size=16 maxlength=16></td></tr>"
                 "<tr><td align=center>Enter Password:</td><td><input type=password name=\"t_password1\" size=16 maxlength=16></td></tr>"
                 "<tr><td align=center>Confirm Password:</td><td><input type=password name=\"t_password2\" size=16 maxlength=16></td></tr>"
                 "<tr><td align=center><A href=\"file:///c:/dm/html/interface_3/browser/useractivation/newusercancelled.htm\">"
                 "<img src=\"file:///c:/dm/html/interface_3/browser/images_03/mcancel.gif\" vspace=0 border=0 alt=\"CANCEL\"></A></td>"
                 "<td align=center><INPUT name=\"btn_ok\" TYPE=SUBMIT VALUE=\" Add to account \"></td></tr>"
                 "</table></center></form></body></HTML>";

            httpd_resp_set_type(req, "text/html");
            httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
            ESP_LOGI(HTTP_UI_TAG, "Served ACT_2 page");
   } else {
       httpd_resp_send(req, "Unhandled HTTP endpoint", HTTPD_RESP_USE_STRLEN);
       ESP_LOGI(HTTP_UI_TAG, "Unhandled HTTP endpoint: %s", req->uri);
   }

   // Always clean up
   if (post_buf) free(post_buf);
   if (fields) free(fields);

   return ESP_OK;
}

// newuserform_handler
// This works with act2 to create a new user. It's here that we can set user options with meta tags
esp_err_t newuserform_handler(httpd_req_t *req) {
    ESP_LOGI(HTTP_UI_TAG, "Handling POST to /cgi-bin/netshark/newuserform");

    ESP_LOGI(HTTP_UI_TAG, "Request: %s %s",
             (req->method == HTTP_GET) ? "GET" :
             (req->method == HTTP_POST) ? "POST" :
             (req->method == HTTP_PUT) ? "PUT" :
             (req->method == HTTP_DELETE) ? "DELETE" : "OTHER",
             req->uri);

    // Only allocate if POST
    char *post_buf = NULL;
    FormField *fields = NULL;
    int field_count = 0;

    if (req->method == HTTP_POST) {
        post_buf = malloc(512);
        fields = malloc(sizeof(FormField) * 10);

        if (!post_buf || !fields) {
            ESP_LOGE(HTTP_UI_TAG, "Out of memory allocating buffers");
            if (post_buf) free(post_buf);
            if (fields) free(fields);
            return ESP_FAIL;
        }

        memset(post_buf, 0, 512);

        int remaining = req->content_len;
        int offset = 0;
        while (remaining > 0) {
            int received = httpd_req_recv(req, post_buf + offset, MIN(remaining, 512 - offset - 1));
            if (received <= 0) {
                if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
                free(post_buf);
                free(fields);
                return ESP_FAIL;
            }
            offset += received;
            remaining -= received;
        }

        ESP_LOGI(HTTP_UI_TAG, "POST raw: %s", post_buf);

        parse_post_data(post_buf, fields, &field_count);

    }

    const char *username1 = get_post_value("t_name1", fields, field_count);
    const char *password1 = get_post_value("t_password1", fields, field_count);
    const char *username2 = get_post_value("t_name2", fields, field_count);
    const char *password2 = get_post_value("t_password2", fields, field_count);

    if ((strcmp(username1, username2) == 0) && (strcmp(password1, password2) == 0)) {
        ESP_LOGI(HTTP_UI_TAG, "Got POST data: t_name1 = %s | t_password1 = %s | t_name2 = %s | t_password2 = %s", username1, password1, username2, password2);
        const char *username_tag = username1;
        const char *password_tag = password1;

        const char *ip = ip4addr_ntoa(netif_ip4_gw(&ppp_netif));
        char *username_encoded = sharkwire_encode(username_tag, ip);
        char *password_encoded = sharkwire_encode(password_tag, ip);
        char *new_user_ok_encoded = sharkwire_encode("NEW_USER_OK", ip);
        char *cangoto_yes_encoded = sharkwire_encode("YES", ip);

        if (!username_encoded || !password_encoded || !new_user_ok_encoded || !cangoto_yes_encoded) {
            httpd_resp_send(req, "Error encoding username, password, or new_user_ok tag", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }

        char *resp = NULL;

        asprintf(&resp,
            "<HTML><head>"
            "<meta height=150 width=540 top=150 left=130 popup=1>"
            "<meta name=\"TARGETRES\" content=\"640x240\">"
            "<meta name=\"WINDOWTYPE\" content=\"POPUP\">"
            "<meta name=\"SHARKWIRE_MAGIC\" content=\"D64A2756_SW_FE62\">"
            "<meta name=\"SHARKWIRE_EMAILNAME\" content=\"%s\">"
            "<meta name=\"SHARKWIRE_EMAILPWD\" content=\"%s\">"
            "<meta name=\"SHARKWIRE_CANGOTO\" content=\"%s\">"
            "<meta name=\"SHARKWIRE_GENERAL_DISCONNECT\" content=\"%s\">"
            "<meta name=\"SHARKWIRE_LASTTAG\" content=\"\">"
            "<title>Sharkwire Username Activation</title></head>"
            "<body><h1>Sharkwire Username Activation</h1></body></HTML>", username_encoded, password_encoded, cangoto_yes_encoded, new_user_ok_encoded);

        if (!resp) {
            httpd_resp_send(req, "Error building ACT2 response HTML", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        ESP_LOGI(HTTP_UI_TAG, "Served ACT_2 response with encoded Username: %s and Password: %s", username_encoded, password_encoded);

        free(new_user_ok_encoded);
        free(username_encoded);
        free(password_encoded);
        free(resp);
    } else {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, "<html><body><center><h1>Username and/or Password didn't match</h1></center></body></html>", HTTPD_RESP_USE_STRLEN);
        ESP_LOGI(HTTP_UI_TAG, "Username or Password did not match");
    }

    return ESP_OK;
}

// http_ui_task
// This is the main task for all of the HTTP services, and it's job is to setup all of the
// handlers for any HTTP request we get from either AP or STA side that need to land locally
void http_ui_task(void *arg) {
    ESP_LOGI(HTTP_UI_TAG, "http_ui started on core %d", xPortGetCoreID());

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 16384;
    config.max_uri_handlers = 16;

    httpd_handle_t server = NULL;
    httpd_start(&server, &config);

    httpd_uri_t config_get_uri = {.uri="/", .method=HTTP_GET, .handler=config_get_handler};
    httpd_uri_t config_post_uri = {.uri="/", .method=HTTP_POST, .handler=config_post_handler};

    // We will handle this in the config_post_handler since it's part of the config page
    httpd_uri_t unbond_ble_device_post_uri = {.uri="/unbond_ble_device", .method=HTTP_POST, .handler=config_post_handler};
    httpd_uri_t home_get_uri = {.uri="/swo/shark.home", .method=HTTP_GET, .handler=home_get_handler};

    // Send Sharkwire act1 GET request to single activation handler
    httpd_uri_t activation1_get_uri = {
        .uri = "/cgi-bin/netshark/act_1",
        .method = HTTP_GET,
        .handler = activation_handler,
    };

    // Send Sharkwire act1 POST request to single activation handler
    httpd_uri_t activation1_post_uri = {
        .uri = "/cgi-bin/netshark/act_1",
        .method = HTTP_POST,
        .handler = activation_handler,
    };

    // Send Sharkwire act2 GET request to single activation handler
    httpd_uri_t activation2_get_uri = {
        .uri = "/cgi-bin/netshark/act_2",
        .method = HTTP_GET,
        .handler = activation_handler,
    };

    // Send Sharkwire act2 POST request to single activation handler
    httpd_uri_t activation2_post_uri = {
        .uri = "/cgi-bin/netshark/act_2",
        .method = HTTP_POST,
        .handler = activation_handler,
    };

    // Send Sharkwire newuserform POST request to newuserform handler
    httpd_uri_t newuserform_post_uri = {
        .uri = "/cgi-bin/netshark/newuserform",
        .method = HTTP_POST,
        .handler = newuserform_handler,
    };

    httpd_uri_t ble_status_uri = {
        .uri = "/ble_status",
        .method = HTTP_GET,
        .handler = ble_status_get_handler,
    };
    httpd_register_uri_handler(server, &ble_status_uri);

    httpd_uri_t menu_uri = {
        .uri = "/swo/menu.htm",
        .method = HTTP_GET,
        .handler = menu_get_handler,
    };
    httpd_register_uri_handler(server, &menu_uri);

    httpd_uri_t content_uri = {
        .uri = "/swo/content.htm",
        .method = HTTP_GET,
        .handler = content_get_handler,
    };
    httpd_register_uri_handler(server, &content_uri);

    httpd_uri_t gamegenie_handler = {
       .uri      = "/cheats/gameshark/n64/*",
       .method   = HTTP_GET,
       .handler  = gamegenie_proxy_handler,
       .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &gamegenie_handler);

    // Register URI handlers
    if (httpd_register_uri_handler(server, &config_get_uri) != ESP_OK) {
        ESP_LOGE(HTTP_UI_TAG, "Failed to register config page GET handler");
    }

    if (httpd_register_uri_handler(server, &config_post_uri) != ESP_OK) {
        ESP_LOGE(HTTP_UI_TAG, "Failed to register config page POST handler");
    }

    if (httpd_register_uri_handler(server, &unbond_ble_device_post_uri) != ESP_OK) {
        ESP_LOGE(HTTP_UI_TAG, "Failed to register unbond_ble_device page POST handler");
    }

    if (httpd_register_uri_handler(server, &home_get_uri) != ESP_OK) {
        ESP_LOGE(HTTP_UI_TAG, "Failed to register /swo/shark.home GET handler");
    }

    if (httpd_register_uri_handler(server, &activation1_get_uri) != ESP_OK) {
        ESP_LOGE(HTTP_UI_TAG, "Failed to register /cgi-bin/netshark/act_1 GET handler");
    }

    if (httpd_register_uri_handler(server, &activation1_post_uri) != ESP_OK) {
        ESP_LOGE(HTTP_UI_TAG, "Failed to register /cgi-bin/netshark/act_1 POST handler");
    }

    if (httpd_register_uri_handler(server, &activation2_get_uri) != ESP_OK) {
        ESP_LOGE(HTTP_UI_TAG, "Failed to register /cgi-bin/netshark/act_2 GET handler");
    }

    if (httpd_register_uri_handler(server, &activation2_post_uri) != ESP_OK) {
        ESP_LOGE(HTTP_UI_TAG, "Failed to register /cgi-bin/netshark/act_2 POST handler");
    }

    if (httpd_register_uri_handler(server, &newuserform_post_uri) != ESP_OK) {
        ESP_LOGE(HTTP_UI_TAG, "Failed to register /cgi-bin/netshark/newuserform POST handler");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}