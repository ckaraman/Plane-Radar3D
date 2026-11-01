#pragma once

/** True when the next boot should show the setup screen first (after credential reset). */
bool wifiShowsSetupScreenOnBoot();
void wifiResetCredentialsAndReboot();
/** Boot flow: connect with UI, if no network is saved, wait for the USB setup page. */
bool wifiSetupConnect();
/** Starts a new try with the saved network and returns at once (no screen): the 5" board
 *  keeps its control screen running while Wi-Fi is away. False if no network is saved. */
bool wifiReconnectInBackground();
/** The setup page is scanning / choosing a network: no background retry for this long
 *  (a retry in progress makes the scan fail, and it would cut a new network's join). */
void wifiPauseBackground(unsigned long ms);
/** Polls the USB setup page; call every loop() iteration. */
void wifiLoop();
bool wifiBootButtonPressed();
/** GPIO + interrupt setup; call once early in setup(). */
void bootButtonInit();
/** Latched short tap (survives blocking HTTP/display work). */
bool bootButtonConsumeTap();
/** Call each loop iteration; triggers WiFi reset on long hold. */
void bootButtonPollLongPress();
