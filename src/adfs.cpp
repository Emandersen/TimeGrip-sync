#include "adfs.hpp"
#include <regex>
#include <stdexcept>
#include <string>

static std::string extract(const std::string& html,
                           const std::string& pattern,
                           const std::string& error_msg) {
    std::regex re(pattern);
    std::smatch m;
    if (!std::regex_search(html, m, re))
        throw std::runtime_error(error_msg);
    return m[1].str();
}

static std::string unescape_html(std::string s) {
    std::string result;
    result.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 5, "&amp;") == 0) {
            result += '&';
            i += 5;
        } else {
            result += s[i++];
        }
    }
    return result;
}

std::unique_ptr<HttpClient> adfs_login(const std::string& email,
                                       const std::string& password) {
    auto session = std::make_unique<HttpClient>(/*follow_redirects=*/true);

    auto r1 = session->get(TG_BASE + "/sso/auth/login",
                           {{"returnUrl", TG_BASE + "/?SSOSuccessful"}});

    if (r1.final_url.find("sts.dsg.dk") == std::string::npos)
        throw std::runtime_error("Expected ADFS redirect, got: " + r1.final_url);

    std::string form_action =
        "https://sts.dsg.dk" +
        unescape_html(extract(r1.body,
                              "action=\"(/adfs/ls/[^\"]+)\"",
                              "Could not find ADFS form action"));

    auto r2 = session->post_form(form_action, {
        {"UserName",   email},
        {"Password",   password},
        {"AuthMethod", "FormsAuthentication"},
        {"Kmsi",       "true"},
    });

    std::string saml_response;
    {
        // Try both attribute orderings
        std::regex re1(R"(name=["']SAMLResponse["'][^>]+value=["']([^"']+)["'])");
        std::regex re2(R"(value=["']([^"']+)["'][^>]+name=["']SAMLResponse["'])");
        std::smatch m;
        if (std::regex_search(r2.body, m, re1) ||
            std::regex_search(r2.body, m, re2)) {
            saml_response = m[1].str();
        } else {
            std::regex err_re(R"(<li[^>]*>\s*(.*?)\s*</li>)");
            std::smatch em;
            std::string msg = "unknown error (wrong credentials?)";
            if (std::regex_search(r2.body, em, err_re)) msg = em[1].str();
            throw std::runtime_error("ADFS auth failed: " + msg);
        }
    }

    std::string relay_state;
    {
        std::regex re1(R"(name=["']RelayState["'][^>]+value=["']([^"']*)["'])");
        std::regex re2(R"(value=["']([^"']*)["'][^>]+name=["']RelayState["'])");
        std::smatch m;
        if (std::regex_search(r2.body, m, re1) ||
            std::regex_search(r2.body, m, re2))
            relay_state = m[1].str();
    }

    std::string acs_url = TG_BASE + "/sso/Saml/AssertionConsumerService";
    {
        std::regex re(R"(<form[^>]+action=["']([^"']+)["'])");
        std::smatch m;
        if (std::regex_search(r2.body, m, re))
            acs_url = m[1].str();
    }

    session->post_form(acs_url, {
        {"SAMLResponse", saml_response},
        {"RelayState",   relay_state},
    });

    if (!is_authenticated(*session, TG_BASE))
        throw std::runtime_error("Session check failed after ADFS auth");

    return session;
}

bool is_authenticated(HttpClient& session, const std::string& base_url) {
    auto r = session.get(base_url + "/webapi/?func=isAuthenticated");
    return r.status == 200;
}
