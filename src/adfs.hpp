#pragma once
#include "http.hpp"
#include <memory>
#include <string>

// Returns an authenticated HttpClient with Timegrip session cookies set.
// Performs the full ADFS SSO chain:
//   GET sso/auth/login → redirect to sts.dsg.dk
//   POST credentials → SAMLResponse
//   POST SAMLResponse to ACS → Timegrip session cookie
std::unique_ptr<HttpClient> adfs_login(const std::string& email,
                                       const std::string& password);

// Quick session validity check.
bool is_authenticated(HttpClient& session, const std::string& base_url);
