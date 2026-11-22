/*
 *  IXSocketConnect.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2018 Machine Zone, Inc. All rights reserved.
 */

#include "IXSocketConnect.h"

#include "IXDNSLookup.h"
#include "IXNetSystem.h"
#include "IXSelectInterrupt.h"
#include "IXSocket.h"
#include "IXUniquePtr.h"
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <vector>

// Android needs extra headers for TCP_NODELAY and IPPROTO_TCP
#ifdef ANDROID
#include <linux/in.h>
#include <linux/tcp.h>
#endif

namespace ix
{
    static constexpr auto kAddressConnectTimeout = std::chrono::seconds(5);

    //
    // This function can be cancelled every 50 ms
    // This is important so that we don't block the main UI thread when shutting down a
    // connection which is already trying to reconnect, and can be blocked waiting for
    // ::connect to respond.
    //
    int SocketConnect::connectToAddress(const struct addrinfo* address,
                                        std::string& errMsg,
                                        const CancellationRequest& isCancellationRequested)
    {
        errMsg = "no error";

        socket_t fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0)
        {
            errMsg = "Cannot create a socket";
            return -1;
        }

        // Set the socket to non blocking mode, so that slow responses cannot
        // block us for too long
        SocketConnect::configure(fd);

        int res = ::connect(fd, address->ai_addr, static_cast<int>(address->ai_addrlen));

        if (res == -1 && !Socket::isWaitNeeded())
        {
            errMsg = strerror(Socket::getErrno());
            Socket::closeSocket(fd);
            return -1;
        }

        for (;;)
        {
            if (isCancellationRequested && isCancellationRequested()) // Must handle timeout as well
            {
                Socket::closeSocket(fd);
                errMsg = "Cancelled";
                return -1;
            }

            int timeoutMs = 10;
            bool readyToRead = false;
            PollResultType pollResult = Socket::poll(readyToRead, timeoutMs, fd, nullptr);

            if (pollResult == PollResultType::Timeout)
            {
                continue;
            }
            else if (pollResult == PollResultType::Error)
            {
                Socket::closeSocket(fd);
                errMsg = std::string("Connect error: ") + strerror(Socket::getErrno());
                return -1;
            }
            else if (pollResult == PollResultType::ReadyForWrite)
            {
                return static_cast<int>(fd);
            }
            else
            {
                Socket::closeSocket(fd);
                errMsg = std::string("Connect error: ") + strerror(Socket::getErrno());
                return -1;
            }
        }
    }

    int SocketConnect::connect(const std::string& hostname,
                               int port,
                               std::string& errMsg,
                               const CancellationRequest& isCancellationRequested)
    {
        //
        // First do DNS resolution
        //
        auto dnsLookup = std::make_shared<DNSLookup>(hostname, port);
        auto res = dnsLookup->resolve(errMsg, isCancellationRequested);
        if (res == nullptr)
        {
            return -1;
        }

        // Alternate address families, so a family that silently drops packets (e.g. broken
        // IPv6) only costs one per-address timeout before the other family is tried
        std::vector<const struct addrinfo*> addresses;
        {
            std::vector<const struct addrinfo*> firstFamily, otherFamily;
            for (const struct addrinfo* address = res.get(); address != nullptr;
                 address = address->ai_next)
            {
                if (address->ai_family == res->ai_family)
                    firstFamily.push_back(address);
                else
                    otherFamily.push_back(address);
            }

            for (size_t i = 0; i < (std::max)(firstFamily.size(), otherFamily.size()); ++i)
            {
                if (i < firstFamily.size()) addresses.push_back(firstFamily[i]);
                if (i < otherFamily.size()) addresses.push_back(otherFamily[i]);
            }
        }

        int sockfd = -1;
        std::string errors;

        for (size_t i = 0; i < addresses.size(); ++i)
        {
            const struct addrinfo* address = addresses[i];

            // The last address gets whatever is left of the caller's timeout
            const bool isLastAddress = (i + 1 == addresses.size());
            const auto deadline = std::chrono::steady_clock::now() + kAddressConnectTimeout;
            bool timedOut = false;

            const CancellationRequest isAttemptCancelled = [&]() -> bool {
                if (isCancellationRequested && isCancellationRequested()) return true;

                if (!isLastAddress && std::chrono::steady_clock::now() > deadline)
                {
                    timedOut = true;
                    return true;
                }

                return false;
            };

            std::string addressErrMsg;
            sockfd = connectToAddress(address, addressErrMsg, isAttemptCancelled);
            if (sockfd != -1)
            {
                errMsg = addressErrMsg;
                break;
            }

            char host[NI_MAXHOST];
            if (getnameinfo(address->ai_addr,
                            static_cast<socklen_t>(address->ai_addrlen),
                            host,
                            sizeof(host),
                            nullptr,
                            0,
                            NI_NUMERICHOST) != 0)
            {
                snprintf(host, sizeof(host), "?");
            }

            if (!errors.empty()) errors += ", ";
            errors += std::string(host) + ": " + (timedOut ? "Timed out" : addressErrMsg);

            if (isCancellationRequested && isCancellationRequested()) break;
        }

        if (sockfd == -1)
        {
            errMsg = errors;
        }

        return sockfd;
    }

    // FIXME: configure is a terrible name
    void SocketConnect::configure(socket_t sockfd)
    {
        // 1. disable Nagle's algorithm
        int flag = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char*) &flag, sizeof(flag));

        // 2. make socket non blocking
#ifdef _WIN32
        unsigned long nonblocking = 1;
        ioctlsocket(sockfd, FIONBIO, &nonblocking);
#else
        fcntl(sockfd, F_SETFL, O_NONBLOCK); // make socket non blocking
#endif

        // 3. (apple) prevent SIGPIPE from being emitted when the remote end disconnect
#ifdef SO_NOSIGPIPE
        int value = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, (void*) &value, sizeof(value));
#endif
    }
} // namespace ix
