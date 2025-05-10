#pragma once

namespace zm {

// Start the plugin pipeline for the given monitor ID
// If no pipeline is configured, this will log a message and return.
void startMonitor(int monitorId);

} // namespace zm
