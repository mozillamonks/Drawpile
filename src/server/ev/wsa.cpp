/*******************************************************************************

   Copyright (C) 2006, 2007 M.K.A. <wyrmchild@users.sourceforge.net>
   
   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   Except as contained in this notice, the name(s) of the above copyright
   holders shall not be used in advertising or otherwise to promote the sale,
   use or other dealings in this Software without prior writeitten authorization.
   
   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.

*******************************************************************************/

#include "wsa.h"

#include "../../shared/templates.h" // fIsSet() and friends

#include "../errors.h"
#include "../socket.errors.h"

#ifndef NDEBUG
	#include <iostream>
	using std::cout;
	using std::endl;
	using std::cerr;
#endif

#include <cassert> // assert()

namespace event {

const bool has_hangup<WSA>::value = true;
const bool has_connect<WSA>::value = true;
const bool has_accept<WSA>::value = true;
const ev_type<WSA>::ev_t read<WSA>::value = FD_READ;
const ev_type<WSA>::ev_t write<WSA>::value = FD_WRITE;
const ev_type<WSA>::ev_t hangup<WSA>::value = FD_CLOSE;
const ev_type<WSA>::ev_t accept<WSA>::value = FD_ACCEPT;
const ev_type<WSA>::ev_t connect<WSA>::value = FD_CONNECT;
const std::string system<WSA>::value("wsa");

const SOCKET invalid_fd<WSA>::value = socket_error::InvalidHandle;

using namespace error;

WSA::WSA()
	: nfds(0), last_event(0)
{
	for (uint i=0; i != max_events; i++)
		w_ev[i] = WSA_INVALID_EVENT;
}

WSA::~WSA()
{
}

// Errors: ENOMEM, WSAENETDOWN, WSAEINPROGRESS
int WSA::wait()
{
	#if defined(DEBUG_EVENTS) and !defined(NDEBUG)
	cout << "wsa.wait()" << endl;
	#endif
	
	assert(fd_to_ev.size() != 0);
	
	/* params: event_count, event_array, wait_for_all, timeout, alertable */
	nfds = WSAWaitForMultipleEvents(fd_to_ev.size(), w_ev, false, _timeout, false);
	if (nfds == WSA_WAIT_FAILED)
	{
		error = WSAGetLastError();
		
		assert(error != WSANOTINITIALISED);
		assert(error != WSA_INVALID_HANDLE);
		assert(error != WSA_INVALID_PARAMETER);
		
		if (error == WSA_NOT_ENOUGH_MEMORY)
			error = OutOfMemory;
		
		return -1;
	}
	else
	{
		switch (nfds)
		{
		case WSA_WAIT_IO_COMPLETION:
		case WSA_WAIT_TIMEOUT:
			return 0;
		default:
			nfds -= WSA_WAIT_EVENT_0;
			return 1;
		}
	}
}

// Errors: WSAENETDOWN
int WSA::add(fd_t fd, ev_t events)
{
	#if defined(DEBUG_EVENTS) and !defined(NDEBUG)
	cout << "wsa.add(fd: " << fd << ")" << endl;
	#endif
	
	assert(fd != socket_error::InvalidHandle);
	
	fSet(events, hangup<WSA>::value);
	
	for (uint i=0; i != max_events; ++i)
	{
		if (w_ev[i] == WSA_INVALID_EVENT)
		{
			w_ev[i] = WSACreateEvent();
			if (!(w_ev[i])) return false;
			
			const int r = WSAEventSelect(fd, w_ev[i], events);
			
			if (r == socket_error::Error)
			{
				error = WSAGetLastError();
				
				assert(error != WSAENOTSOCK);
				assert(error != WSAEINVAL);
				assert(error != WSANOTINITIALISED);
				
				return false;
			}
			
			// update fd_to_ev map and fdl array
			fd_to_ev[fd] = i;
			fdl[i] = fd;
			
			if (i > last_event)
				last_event = i;
			
			return true;
		}
	}
	
	#ifndef NDEBUG
	cerr << "Event system overloaded!" << endl;
	#endif
	
	return false;
}

// Errors: WSAENETDOWN
int WSA::modify(fd_t fd, ev_t events)
{
	#if defined(DEBUG_EVENTS) and !defined(NDEBUG)
	cout << "wsa.modify(fd: " << fd << ")" << endl;
	#endif
	
	assert(fd != socket_error::InvalidHandle);
	
	fSet(events, hangup<WSA>::value);
	
	const ev_iter fi(fd_to_ev.find(fd));
	assert(fi != fd_to_ev.end());
	assert(fi->second < max_events);
	const uint i = fi->second;
	
	const int r = WSAEventSelect(fd, w_ev[i], events);
	
	if (r == socket_error::Error)
	{
		error = WSAGetLastError();
		
		assert(error != WSAENOTSOCK);
		assert(error != WSAEINVAL);
		assert(error != WSANOTINITIALISED);
		
		return false;
	}
	
	return 0;
}

int WSA::remove(fd_t fd)
{
	#if defined(DEBUG_EVENTS) and !defined(NDEBUG)
	cout << "wsa.remove(fd: " << fd << ")" << endl;
	#endif
	
	assert(fd != socket_error::InvalidHandle);
	
	const ev_iter fi(fd_to_ev.find(fd));
	assert(fi != fd_to_ev.end());
	
	assert(fi->second < max_events);
	WSACloseEvent(w_ev[fi->second]);
	
	w_ev[fi->second] = WSA_INVALID_EVENT;
	
	fdl[fi->second] = socket_error::InvalidHandle;
	fd_to_ev.erase(fi);
	
	// find last event
	for (; w_ev[last_event] == WSA_INVALID_EVENT; --last_event)
		if (last_event == 0) break;
	
	return true;
}

bool WSA::getEvent(fd_t &fd, ev_t &events)
{
	WSANETWORKEVENTS set;
	
	events = 0;
	for (; nfds <= last_event; ++nfds)
	{
		if (w_ev[nfds] != WSA_INVALID_EVENT)
		{
			fd = fdl[nfds];
			assert(fd != socket_error::InvalidHandle);
			
			const int r = WSAEnumNetworkEvents(fd, w_ev[nfds], &set);
			
			if (r == socket_error::Error)
			{
				error = WSAGetLastError();
				
				switch (error)
				{
				case WSAECONNRESET: // reset by remote
				case WSAECONNABORTED: // connection aborted
				case WSAETIMEDOUT: // connection timed-out
				case WSAENETUNREACH: // network unreachable
				case WSAECONNREFUSED: // connection refused/rejected
					events = hangup<WSA>::value;
					break;
				case WSAENOBUFS: // out of network buffers
				case WSAENETDOWN: // network sub-system failure
				case WSAEINPROGRESS: // something's in progress
					break;
				default:
					#ifndef NDEBUG
					cerr << "Event(wsa).getEvent() - unknown error: " << error << endl;
					#endif
					break;
				}
			}
			else
				events = set.lNetworkEvents;
			
			if (events != 0)
			{
				++nfds;
				return true;
			}
			#ifndef NDEBUG
			else
			{
				cout << "- No events triggered for FD: " << fd << endl;
			}
			#endif
		}
	}
	
	return false;
}

void WSA::timeout(uint msecs)
{
	_timeout = msecs;
}

} // namespace:event
