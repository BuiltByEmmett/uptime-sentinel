// HTML pieces shared by the setup portal and the web dashboard.
#pragma once
#include <Arduino.h>
#include <WebServer.h>

namespace html {

String escape(const String &text);

// Opening of every page: head, shared stylesheet, <body> and the heading.
String pageStart(const char *title);
extern const char PAGE_END[];

// Form fields for timezone and ping targets, pre-filled from saved settings.
String optionFields();

// Reads those fields back from a submitted form. On a problem, returns false
// and puts a sentence for the user in `error`.
bool readOptions(WebServer &server, String &tz, String &target1, String &target2,
                 String &error);

// Form fields for setting, changing or removing the admin password.
String adminFields();

// Reads them back. `change` is false when the user left them alone; an empty
// `password` with `change` set means remove it.
bool readAdmin(WebServer &server, String &password, bool &change, String &error);

}  // namespace html
