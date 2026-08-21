#pragma once

// LanguageSync - tells the server which language this client shows.
//
// 🔴 Why this exists (21/08/2026, the move to an international audience):
// the game had TWO language switches and they did not talk to each other.
// The client switch (ESC → Options) changed the UI and item names locally;
// the server kept sending its messages in Hebrew until the player found the
// hidden /language chat command - and even that was forgotten on logout
// (nothing ever wrote Account.LanguageIsoCode; fixed server-side the same
// day). A player and his friend searched half an hour and found neither.
//
// This closes the loop from the client side: whenever the client's language
// changes, it announces it to the server over the EXISTING chat channel -
// an automatic "/language he|en" message. No new packet, no protocol
// change, and an old server simply answers "unknown command" harmlessly.
//
// The announcement is deduplicated through config.ini (LanguageSynced), so
// the Hebrew-speaking player base never sends anything at all - only an
// actual switch triggers one message, once.

// Announces the client's language to the server if it changed since the
// last announcement. Safe to call whenever - it does nothing when not in
// game or when there is nothing new to say.
void SyncLanguageWithServer();
