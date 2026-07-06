#include "http.hpp"
#include <curl/curl.h>
#include <sstream>
#include <stdexcept>

HttpClient::HttpClient(bool follow_redirects) {
    curl_ = curl_easy_init();
    if (!curl_) throw std::runtime_error("curl_easy_init failed");

    auto* c = static_cast<CURL*>(curl_);
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");          // enable cookie engine
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, follow_redirects ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 20L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    set_user_agent(
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36");
}

HttpClient::~HttpClient() {
    if (curl_) curl_easy_cleanup(static_cast<CURL*>(curl_));
}

void HttpClient::set_user_agent(const std::string& ua) {
    curl_easy_setopt(static_cast<CURL*>(curl_), CURLOPT_USERAGENT, ua.c_str());
}

std::size_t HttpClient::write_cb(char* ptr, std::size_t size,
                                 std::size_t nmemb, void* ud) {
    auto* body = static_cast<std::string*>(ud);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string HttpClient::url_encode(const std::string& s) const {
    char* enc = curl_easy_escape(static_cast<CURL*>(curl_),
                                 s.c_str(), static_cast<int>(s.size()));
    std::string result(enc);
    curl_free(enc);
    return result;
}

std::string HttpClient::build_query(const Params& p) const {
    std::string q;
    for (auto& [k, v] : p) {
        if (!q.empty()) q += '&';
        q += url_encode(k) + '=' + url_encode(v);
    }
    return q;
}

HttpResponse HttpClient::get(const std::string& url, const Params& params) {
    auto* c = static_cast<CURL*>(curl_);

    std::string full_url = url;
    if (!params.empty()) {
        full_url += (full_url.find('?') == std::string::npos ? '?' : '&');
        full_url += build_query(params);
    }

    std::string body;
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_URL, full_url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("curl GET failed: ") +
                                 curl_easy_strerror(rc));

    HttpResponse resp;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &resp.status);
    char* final_url = nullptr;
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &final_url);
    if (final_url) resp.final_url = final_url;
    resp.body = std::move(body);
    return resp;
}

HttpResponse HttpClient::post_form(const std::string& url,
                                   const std::vector<FormField>& fields) {
    auto* c = static_cast<CURL*>(curl_);

    std::string post_data;
    for (auto& f : fields) {
        if (!post_data.empty()) post_data += '&';
        post_data += url_encode(f.name) + '=' + url_encode(f.value);
    }

    std::string body;
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("curl POST failed: ") +
                                 curl_easy_strerror(rc));

    HttpResponse resp;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &resp.status);
    char* final_url = nullptr;
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &final_url);
    if (final_url) resp.final_url = final_url;
    resp.body = std::move(body);
    return resp;
}

HttpResponse HttpClient::post_json(const std::string& url,
                                   const std::string& json_body,
                                   const Headers& extra_headers) {
    auto* c = static_cast<CURL*>(curl_);

    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    for (auto& [k, v] : extra_headers)
        hdrs = curl_slist_append(hdrs, (k + ": " + v).c_str());

    std::string body;
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);

    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hdrs);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, nullptr);

    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("curl POST JSON failed: ") +
                                 curl_easy_strerror(rc));

    HttpResponse resp;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &resp.status);
    char* final_url = nullptr;
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &final_url);
    if (final_url) resp.final_url = final_url;
    resp.body = std::move(body);
    return resp;
}
