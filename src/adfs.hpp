#pragma once
#include "http.hpp"
#include <memory>
#include <string>

// Returns an authenticated HttpClient with Timegrip session cookies set.
std::unique_ptr<HttpClient> adfs_login(const std::string& email,
                                       const std::string& password);

// Quick session validity check.
bool is_authenticated(HttpClient& session, const std::string& base_url);
