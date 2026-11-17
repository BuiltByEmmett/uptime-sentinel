// /report: a plain, printable page for one calendar month, meant to be saved
// as a PDF and sent to an ISP. Summary figures, then every ISP outage with
// start, end and duration. Built from the outage log. It never includes the
// public IP address or anything else about the local network.
#pragma once
#include <Arduino.h>
#include <WebServer.h>

namespace report {

void send(WebServer &server);  // reads ?month=YYYY-MM; the current month without it

}  // namespace report
