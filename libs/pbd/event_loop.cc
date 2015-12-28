/*
    Copyright (C) 2012 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include "pbd/compose.h"
#include "pbd/event_loop.h"
#include "pbd/error.h"
#include "pbd/stacktrace.h"

#include "i18n.h"

using namespace PBD;
using namespace std;

static void do_not_delete_the_loop_pointer (void*) { }

Glib::Threads::Private<EventLoop> EventLoop::thread_event_loop (do_not_delete_the_loop_pointer);

Glib::Threads::RWLock EventLoop::thread_buffer_requests_lock;
EventLoop::ThreadRequestBufferList EventLoop::thread_buffer_requests;
EventLoop::RequestBufferSuppliers EventLoop::request_buffer_suppliers;

EventLoop::EventLoop (string const& name)
	: _name (name)
{
}

EventLoop*
EventLoop::get_event_loop_for_thread()
{
	return thread_event_loop.get ();
}

void
EventLoop::set_event_loop_for_thread (EventLoop* loop)
{
	thread_event_loop.set (loop);
}

void*
EventLoop::invalidate_request (void* data)
{
        InvalidationRecord* ir = (InvalidationRecord*) data;

	/* Some of the requests queued with an EventLoop may involve functors
	 * that make method calls to objects whose lifetime is shorter
	 * than the EventLoop's. We do not want to make those calls if the
	 * object involve has been destroyed. To prevent this, we
	 * provide a way to invalidate those requests when the object is
	 * destroyed.
	 *
	 * An object was passed to __invalidator() which added a callback to
	 * EventLoop::invalidate_request() to its "notify when destroyed"
	 * list. __invalidator() returned an InvalidationRecord that has been
	 * to passed to this function as data.
	 *
	 * The object is currently being destroyed and so we want to
	 * mark all requests involving this object that are queued with
	 * any EventLoop as invalid.
	 *
	 * As of April 2012, we are usign sigc::trackable as the base object
	 * used to queue calls to ::invalidate_request() to be made upon
	 * destruction, via its ::add_destroy_notify_callback() API. This is
	 * not necessarily ideal, but it is very close to precisely what we
	 * want, and many of the objects we want to do this with already
	 * inherit (indirectly) from sigc::trackable.
	 */

        if (ir->event_loop) {
		Glib::Threads::Mutex::Lock lm (ir->event_loop->slot_invalidation_mutex());
		for (list<BaseRequestObject*>::iterator i = ir->requests.begin(); i != ir->requests.end(); ++i) {
			(*i)->valid = false;
			(*i)->invalidation = 0;
		}
		delete ir;
        }

        return 0;
}

vector<EventLoop::ThreadBufferMapping>
EventLoop::get_request_buffers_for_target_thread (const std::string& target_thread)
{
	vector<ThreadBufferMapping> ret;
	Glib::Threads::RWLock::WriterLock lm (thread_buffer_requests_lock);

	for (ThreadRequestBufferList::const_iterator x = thread_buffer_requests.begin();
	     x != thread_buffer_requests.end(); ++x) {

		if (x->second.target_thread_name == target_thread) {
			ret.push_back (x->second);
		}
	}

	return ret;
}

void
EventLoop::register_request_buffer_factory (const string& target_thread_name,
                                            void* (*factory)(uint32_t))
{

	RequestBufferSupplier trs;
	trs.name = target_thread_name;
	trs.factory = factory;

	{
		Glib::Threads::RWLock::WriterLock lm (thread_buffer_requests_lock);
		request_buffer_suppliers.push_back (trs);
	}
}

void
EventLoop::pre_register (const string& emitting_thread_name, uint32_t num_requests)
{
	/* Threads that need to emit signals "towards" other threads, but with
	   RT safe behavior may be created before the receiving threads
	   exist. This makes it impossible for them to use the
	   ThreadCreatedWithRequestSize signal to notify receiving threads of
	   their existence.

	   This function creates a request buffer for them to use with
	   the (not yet) created threads, and stores it where the receiving
	   thread can find it later.
	 */

	ThreadBufferMapping mapping;
	Glib::Threads::RWLock::ReaderLock lm (thread_buffer_requests_lock);

	for (RequestBufferSuppliers::iterator trs = request_buffer_suppliers.begin(); trs != request_buffer_suppliers.end(); ++trs) {

		if (!trs->factory) {
			/* no factory - no request buffer required or expected */
			continue;
		}

		if (emitting_thread_name == trs->name) {
			/* no need to register an emitter with itself */
			continue;
		}

		mapping.emitting_thread = pthread_self();
		mapping.target_thread_name = trs->name;

		/* Allocate a suitably sized request buffer. This will set the
		 * thread-local variable that holds a pointer to this request
		 * buffer.
		 */
		mapping.request_buffer = trs->factory (num_requests);

		/* now store it where the receiving thread (trs->name) can find
		   it if and when it is created. (Discovery happens in the
		   AbstractUI constructor. Note that if
		*/

		/* make a key composed of the emitter and receiver thread names */

		string key = emitting_thread_name;
		key += '/';
		key +=  mapping.target_thread_name;

		/* if the emitting thread was killed and recreated (with the
		 * same name), this will replace the entry in
		 * thread_buffer_requests. The old entry will be lazily deleted
		 * when the target thread finds the request buffer and realizes
		 * that it is dead.
		 *
		 * If the request buffer is replaced before the target thread
		 * ever finds the dead version, we will leak the old request
		 * buffer.
		 */

		thread_buffer_requests[key] = mapping;
	}
}
