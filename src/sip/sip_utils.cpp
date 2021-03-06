/*
 *  Copyright (C) 2004-2020 Savoir-faire Linux Inc.
 *
 *  Author: Tristan Matthews <tristan.matthews@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 */

#include "sip_utils.h"
#include "logger.h"
#include "utf8_utils.h"

#include <pjsip.h>
#include <pjsip_ua.h>
#include <pjlib-util.h>
#include <pjnath.h>
#include <pjnath/stun_config.h>
#include <pj/string.h>
#include <pjsip/sip_types.h>
#include <pjsip/sip_uri.h>
#include <pj/list.h>

#ifndef _WIN32
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <vector>
#include <algorithm>

namespace jami {
namespace sip_utils {

std::string
PjsipErrorCategory::message(int condition) const
{
    std::string err_msg;
    err_msg.reserve(PJ_ERR_MSG_SIZE);
    err_msg.resize(pj_strerror(condition, &err_msg[0], err_msg.capacity()).slen);
    return err_msg;
}

std::string
fetchHeaderValue(pjsip_msg* msg, const std::string& field)
{
    pj_str_t name = pj_str((char*) field.c_str());
    pjsip_generic_string_hdr* hdr = static_cast<pjsip_generic_string_hdr*>(
        pjsip_msg_find_hdr_by_name(msg, &name, NULL));

    if (!hdr)
        return "";

    std::string value(hdr->hvalue.ptr, hdr->hvalue.slen);

    size_t pos = value.find('\n');

    if (pos != std::string::npos)
        return value.substr(0, pos);
    else
        return "";
}

pjsip_route_hdr*
createRouteSet(const std::string& route, pj_pool_t* hdr_pool)
{
    pjsip_route_hdr* route_set = pjsip_route_hdr_create(hdr_pool);

    std::string host;
    int port = 0;
    size_t found = route.find(':');
    if (found != std::string::npos) {
        host = route.substr(0, found);
        port = atoi(route.substr(found + 1, route.length() - found).c_str());
    } else
        host = route;

    pjsip_route_hdr* routing = pjsip_route_hdr_create(hdr_pool);
    pjsip_sip_uri* url = pjsip_sip_uri_create(hdr_pool, 0);
    url->lr_param = 1;
    routing->name_addr.uri = (pjsip_uri*) url;
    pj_strdup2(hdr_pool, &url->host, host.c_str());
    url->port = port;

    JAMI_DBG("Adding route %s", host.c_str());
    pj_list_push_back(route_set, pjsip_hdr_clone(hdr_pool, routing));

    return route_set;
}

std::string
parseDisplayName(const pjsip_name_addr* sip_name_addr)
{
    if (not sip_name_addr->display.ptr or not sip_name_addr->display.slen)
        return {};

    std::string displayName {sip_name_addr->display.ptr,
                             static_cast<size_t>(sip_name_addr->display.slen)};

    // Filter out invalid UTF-8 characters to avoid getting kicked from D-Bus
    if (not utf8_validate(displayName))
        return utf8_make_valid(displayName);

    return displayName;
}

std::string
parseDisplayName(const pjsip_from_hdr* header)
{
    // PJSIP return a pjsip_name_addr for To, From and Contact headers
    return parseDisplayName(reinterpret_cast<pjsip_name_addr*>(header->uri));
}

std::string
parseDisplayName(const pjsip_contact_hdr* header)
{
    // PJSIP return a pjsip_name_addr for To, From and Contact headers
    return parseDisplayName(reinterpret_cast<pjsip_name_addr*>(header->uri));
}

void
stripSipUriPrefix(std::string& sipUri)
{
    // Remove sip: prefix
    static const char SIP_PREFIX[] = "sip:";
    size_t found = sipUri.find(SIP_PREFIX);

    if (found != std::string::npos)
        sipUri.erase(found, (sizeof SIP_PREFIX) - 1);

    // URI may or may not be between brackets
    found = sipUri.find('<');
    if (found != std::string::npos)
        sipUri.erase(found, 1);

    found = sipUri.find('@');

    if (found != std::string::npos)
        sipUri.erase(found);

    found = sipUri.find('>');
    if (found != std::string::npos)
        sipUri.erase(found, 1);
}

std::string
getHostFromUri(const std::string& sipUri)
{
    std::string hostname(sipUri);
    size_t found = hostname.find('@');
    if (found != std::string::npos)
        hostname.erase(0, found + 1);

    found = hostname.find('>');
    if (found != std::string::npos)
        hostname.erase(found, 1);

    return hostname;
}

void
addContactHeader(const pj_str_t* contact_str, pjsip_tx_data* tdata)
{
    pjsip_contact_hdr* contact = pjsip_contact_hdr_create(tdata->pool);
    contact->uri = pjsip_parse_uri(tdata->pool,
                                   contact_str->ptr,
                                   contact_str->slen,
                                   PJSIP_PARSE_URI_AS_NAMEADDR);
    // remove old contact header (if present)
    pjsip_msg_find_remove_hdr(tdata->msg, PJSIP_H_CONTACT, NULL);
    pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr*) contact);
}

void
addUserAgenttHeader(const std::string& userAgent, pjsip_tx_data* tdata)
{
    if (tdata == nullptr)
        return;

    pj_str_t pjUserAgent = pj_str((char*)userAgent.c_str());
    constexpr pj_str_t STR_USER_AGENT = jami::sip_utils::CONST_PJ_STR("User-Agent");

    // Do nothing if user-agent header is present.
    pjsip_hdr* hdr = static_cast<pjsip_hdr*>(pjsip_msg_find_hdr_by_name(tdata->msg, &STR_USER_AGENT, nullptr));
    if (hdr != nullptr) {
        return;
    }

    // Add Header
    hdr = reinterpret_cast<pjsip_hdr*>(
        pjsip_user_agent_hdr_create(tdata->pool, &STR_USER_AGENT, &pjUserAgent));

    if (hdr != nullptr) {
        JAMI_DBG("Add header to SIP message: \"%.*s: %.*s\"", (int) hdr->name.slen, hdr->name.ptr,
           (int) pjUserAgent.slen, pjUserAgent.ptr);
        pjsip_msg_add_hdr(tdata->msg, hdr);
    }
}

void logMessageHeaders(const pjsip_hdr* hdr_list)
{
    const pjsip_hdr* hdr = hdr_list->next;
    const pjsip_hdr* end = hdr_list;
    std::string msgHdrStr("Message headers:\n");
    for (; hdr != end; hdr = hdr->next) {
        char buf[1024];
        int size = pjsip_hdr_print_on((void*)hdr, buf, sizeof(buf));
        if (size > 0) {
            msgHdrStr.append(buf, size);
            msgHdrStr.push_back('\n');
        }
    }

    JAMI_INFO("%.*s", (int) msgHdrStr.size(), msgHdrStr.c_str());
}

std::string
sip_strerror(pj_status_t code)
{
    char err_msg[PJ_ERR_MSG_SIZE];
    auto ret = pj_strerror(code, err_msg, sizeof err_msg);
    return std::string {ret.ptr, ret.ptr+ret.slen};
}

void
register_thread()
{
    if (!pj_thread_is_registered()) {
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8) || defined _MSC_VER
        static thread_local pj_thread_desc desc;
        static thread_local pj_thread_t* this_thread;
#else
        static __thread pj_thread_desc desc;
        static __thread pj_thread_t* this_thread;
#endif
        pj_thread_register(NULL, desc, &this_thread);
        JAMI_DBG("Registered thread %p (0x%X)", this_thread, pj_getpid());
    }
}

void
sockaddr_to_host_port(pj_pool_t* pool, pjsip_host_port* host_port, const pj_sockaddr* addr)
{
    host_port->host.ptr = (char*) pj_pool_alloc(pool, PJ_INET6_ADDRSTRLEN + 4);
    pj_sockaddr_print(addr, host_port->host.ptr, PJ_INET6_ADDRSTRLEN + 4, 0);
    host_port->host.slen = pj_ansi_strlen(host_port->host.ptr);
    host_port->port = pj_sockaddr_get_port(addr);
}

} // namespace sip_utils
} // namespace jami
