/*
 * Copyright (C) 2019 Medusalix
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "utils/log.h"
#include "utils/reader.h"
#include "dongle/usb.h"
#include "dongle/dongle.h"

#include <cstdlib>
#include <cstring>
#include <csignal>
#include <sys/file.h>
#include <sys/signalfd.h>

bool acquireLock()
{
    // Open lock file, read and writable by all users
    int file = open(LOCK_FILE, O_CREAT | O_RDWR, 0666);

    if (flock(file, LOCK_EX | LOCK_NB))
    {
        if (errno == EWOULDBLOCK)
        {
            Log::error("Another instance of xow is already running");
        }

        else
        {
            Log::error("Error creating lock file: %s", strerror(errno));
        }

        return false;
    }

    return true;
}

bool run()
{
    sigset_t signalMask;

    sigemptyset(&signalMask);
    sigaddset(&signalMask, SIGINT);
    sigaddset(&signalMask, SIGTERM);
    sigaddset(&signalMask, SIGUSR1);

    // Block signals for all USB threads
    if (pthread_sigmask(SIG_BLOCK, &signalMask, nullptr) < 0)
    {
        Log::error("Error blocking signals: %s", strerror(errno));

        return false;
    }

    UsbDeviceManager manager;

    // Unblock signals for current thread to allow interruption
    if (pthread_sigmask(SIG_UNBLOCK, &signalMask, nullptr) < 0)
    {
        Log::error("Error unblocking signals: %s", strerror(errno));

        return false;
    }

    // Bind USB device termination to signal reader interruption
    InterruptibleReader signalReader;
    UsbDevice::Terminate terminate = std::bind(
        &InterruptibleReader::interrupt,
        &signalReader
    );
    std::unique_ptr<UsbDevice> device = manager.getDevice({
        { DONGLE_VID, DONGLE_PID_OLD },
        { DONGLE_VID, DONGLE_PID_NEW }
    }, terminate);

    // Block signals and pass them to the signalfd
    if (pthread_sigmask(SIG_BLOCK, &signalMask, nullptr) < 0)
    {
        Log::error("Error blocking signals: %s", strerror(errno));

        return false;
    }

    int file = signalfd(-1, &signalMask, 0);

    if (file < 0)
    {
        Log::error("Error creating signal file: %s", strerror(errno));

        return false;
    }

    signalReader.prepare(file);

    Dongle dongle(std::move(device));
    signalfd_siginfo info = {};

    while (signalReader.read(&info, sizeof(info)))
    {
        uint32_t type = info.ssi_signo;

        if (type == SIGINT || type == SIGTERM)
        {
            break;
        }

        if (type == SIGUSR1)
        {
            Log::debug("User signal received");

            dongle.setPairingStatus(true);
        }
    }

    Log::info("Shutting down...");

    return true;
}

int main()
{
    Log::init();
    Log::info("xow %s ©Severin v. W.", VERSION);

    if (!acquireLock())
    {
        return EXIT_FAILURE;
    }

    if (!run())
    {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
