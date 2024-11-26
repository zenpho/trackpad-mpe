// ==============================================================================
//	fingerpinger.cpp
//	
//	tracking fingers on a macbook trackpad
//	
//	Authors:	Michael & Max Egger
//	Copyright:	2009 [ a n y m a ]
//	Website:	www.anyma.ch
//
//  Based on: 	http://www.steike.com/code/multitouch/
//	
//	License:	GNU GPL 2.0 www.gnu.org
//	
//	Version:	2009-06-01
// ==============================================================================



#include "ext.h"  				// you must include this - it contains the external object's link to available Max functions
#include "ext_obex.h"
#include "ext_common.h"
#include <assert.h>
#include <list>
#include <map>
#include <vector>
#include <algorithm>
#include <CoreFoundation/CoreFoundation.h>
#include "MultitouchSupport.h"
#include <ext_critical.h>
using namespace std;

#define DEBUG_INSTANCE_MAP 1


// ==============================================================================
// Helper structure for one frame of finger data, with simple reference counting
// ------------------------------------------------------------------------------

typedef struct _FingeFrame {
	int refcount;
	int size;
	Finger finger[0];	// Variable size, number of elements given in 'size'
} FingerFrame;

static FingerFrame* make_frame(int nFingers, Finger* data)
{
	FingerFrame* frame = (FingerFrame*)::malloc(sizeof(FingerFrame) + nFingers*sizeof(Finger));
	if (!frame) return 0;	// allocation failed
	
	frame->refcount = 1;
	frame->size = nFingers;
	::memcpy(frame->finger, data, nFingers*sizeof(Finger));
	return frame;
}

static FingerFrame* retain_frame(FingerFrame* frame) {
	++frame->refcount;
	return frame;
}

static void release_frame(FingerFrame* frame)
{
	assert(frame->refcount >= 1);
	if (--frame->refcount <= 0) {
		::free(frame);
	}
}


// Can't use the global critical section, because sometimes it's locked from the
// outside during a free. This in turn leads to dead locks in the multitouch
// callback function. Instead, we create our own in main(), which wee will leak
// because there's no place where we can free it.
t_critical crit;


// ==============================================================================
// RAII Helpers for critical section handling
// ------------------------------------------------------------------------------
namespace {
	struct lock_critical {
		lock_critical() { critical_enter(crit); }
		~lock_critical() { critical_exit(crit); }
	};

	struct unlock_critical {
		unlock_critical() { critical_exit(crit); }
		~unlock_critical() { critical_enter(crit); }
	};
}

// ==============================================================================
// Our External's Memory structure
// ------------------------------------------------------------------------------

typedef std::list<FingerFrame*> FingerQueue;

typedef struct _multitouch				// defines our object's internal variables for each instance in a patch
{
	t_object 		p_ob;				// object header - ALL max external MUST begin with this...
	void 			*out_list;			// outlet for finger data		
	void			*out_bang;			// bang out for start of frame
	void			*evt_qelem;			// qelem to trigger output of finger events
	MTDeviceRef 	dev;				// reference to the multitouch device
	FingerQueue		*evts;				// queue of finger frames we still have to report
	
} t_multitouch;

static t_class *s_multitouch_class = NULL;	// global pointer to the object class - so max can reference the object


typedef std::list<t_multitouch*> InstanceList;
typedef std::map<MTDeviceRef, InstanceList> InstanceMap;

/*! Maps each open multitouch device to a list of fingerpinger instances.
    It is used in the callback function to find out which objects need to
    output the data. Declared as a pointer so we don't have to rely on Max
	calling the constructor. */
static InstanceMap* instances = NULL;	
													

// ==============================================================================
// Function Prototypes
// ------------------------------------------------------------------------------

extern "C" {int C74_EXPORT main(void);}

static void *multitouch_new(t_symbol *s);
static void multitouch_free(t_multitouch *x);
static void multitouch_int(t_multitouch *x,long n);
static void multitouch_output(t_multitouch *x);

static void multitouch_open_device(t_multitouch* x, MTDeviceRef dev);
static void multitouch_close_device(t_multitouch* x, MTDeviceRef dev);

// ==============================================================================
// Implementation
// ------------------------------------------------------------------------------


//--------------------------------------------------------------------------
// - Callback
//--------------------------------------------------------------------------

static int callback(MTDeviceRef device, Finger *data, int nFingers, double timestamp, int frame) {
	// critical section 0 must be locked at any time we access 'instances' or a t_multitouch
	lock_critical lock;
	if (instances == NULL) return 0;	// May happen if we are currently closing the last device
	
	InstanceMap::iterator it = instances->find(device);
	if (it == instances->end()) return 0;	// May happen if we are currently closing the device
	
	// Copy the finger data into a new frame structure
	FingerFrame* new_frame = make_frame(nFingers, data);
	if (!new_frame) return 0;
	
	// Append the frame data to the queues of all concerned fingerpinger instances
	for (InstanceList::iterator ob = it->second.begin(); ob != it->second.end(); ++ob) {
		(*ob)->evts->push_back(retain_frame(new_frame));
		qelem_set((*ob)->evt_qelem);
	}
	
	// release our reference to the frame we got from make_frame()
	release_frame(new_frame);
	
	return 0;
}

static void multitouch_output(t_multitouch *x) {
	t_atom myList[12];
	
	// The while condition must be inside a critical section.
	lock_critical lock;
	while (!x->evts->empty()) {
		FingerFrame* frame = NULL;
		{
			unlock_critical unlock; // Release critical section while doing output
			outlet_bang(x->out_bang);
		
			// Once created, frames are read-only, so as long as we have a reference
			// on it we can safely access the frame outside of the critical section
			frame = x->evts->front();
			for (int i = 0; i < frame->size; ++i) {
				Finger& f = frame->finger[i];
				 atom_setfloat(myList, i);
				 atom_setfloat(myList + 1 , f.frame);
				 atom_setfloat(myList + 2 , f.angle);
				 atom_setfloat(myList + 3 , f.majorAxis);
				 atom_setfloat(myList + 4 , f.minorAxis);
				 atom_setfloat(myList + 5 , f.normalized.pos.x);
				 atom_setfloat(myList + 6 , f.normalized.pos.y);
				 atom_setfloat(myList + 7 , f.normalized.vel.x);
				 atom_setfloat(myList + 8 , f.normalized.vel.y);
				 atom_setfloat(myList + 9 , f.identifier);
				 atom_setfloat(myList + 10, f.state);
				 atom_setfloat(myList + 11, f.size);
				outlet_list(x->out_list, 0L, 12, myList);
			}
			// Re-acquire critical section to protect manipulation of frame refcount,
			// event queue updating and 'while' condition. 
		}
		release_frame(frame);
		x->evts->pop_front();
	}
}

//--------------------------------------------------------------------------
// - On / Off
//--------------------------------------------------------------------------
#ifdef DEBUG_INSTANCE_MAP
static void print_instances(t_multitouch const* x)
{
	post("instances: ");
	if (!instances) {
		post("NULL");
		return;
	}
	for (InstanceMap::const_iterator it = instances->begin(); it != instances->end(); ++it) {
		post("%p ->", it->first);
		for (InstanceList::const_iterator ob = it->second.begin(); ob != it->second.end(); ++ob)
			post(" %p,", (*ob));
	}
}
#endif

static void multitouch_open_device(t_multitouch* x, MTDeviceRef dev)
{
	// NOTE: We assume that we are in critical section 0 when entering this function!
	
	assert(dev);
	// Create instance map if there is none.
	if (!instances) instances = new InstanceMap;
	
	// Check if this device is already in the map.
	InstanceMap::iterator i = instances->find(dev);
	if (i == instances->end()) {
		// If not, open the device and insert it into the map
		CFRetain(dev);
		(*instances)[dev] = InstanceList(1, x);
		x->dev = dev;
		
		// Exit critical section during open to avoid potential deadlocks in callback;
		// WARNING: we must assume that all iterators become invalid at this point
		// because another thread might modify 'instances' at any time!
		{
			unlock_critical unlock;		
			MTRegisterContactFrameCallback(dev, callback);
			MTDeviceStart(dev);
		}
	}
	else {
		// If the device is already in the list, just add the fingerpinger instance
		// to its list of client devices.
		i->second.push_back(x);
		x->dev = dev;
	}
#ifdef DEBUG_INSTANCE_MAP
	print_instances(x);
#endif
}

static void multitouch_close_device(t_multitouch* x, MTDeviceRef dev)
{
	// NOTE: We assume that we are in critical section 0 when entering this function!
	
	x->dev = NULL;
	if (!dev) return;
	assert(instances);
	if (!instances) return;
	
	// Find this instance in the device map.
	InstanceMap::iterator i = instances->find(dev);
	assert(i != instances->end());
	
	if (i != instances->end()) {
		// Find this instance in the list of clients of the device.
		InstanceList& obs = i->second;
		InstanceList::iterator ob = std::find(obs.begin(), obs.end(), x);
		assert(ob != obs.end());
		
		if (ob != obs.end()) {
			if (obs.size() > 1) {
				// If other instances still use this device, just remove
				// the instance from the client list.
				obs.erase(ob);
			}
			else {
				// If this is the last instance still using this device, close
				// the device and remove it completely from the instance map.
				instances->erase(i);

				// Exit critical section during close to avoid potential deadlocks in callback;
				// WARNING: we must assume that all iterators become invalid at this point,
				// because another thread might modify 'instances' at any time!
				{
					unlock_critical unlock;
					MTRegisterContactFrameCallback(dev, NULL);
					MTDeviceStop(dev);
					MTDeviceRelease(dev);
				}
			}
		}
	}
#ifdef DEBUG_INSTANCE_MAP
	print_instances(x);
#endif
	if (instances->empty()) {
		delete instances;
		instances = NULL;
	}
}

static void multitouch_int(t_multitouch *x, long n) {
	if (n < 0) n = 0;
	lock_critical lock;
	if (x->dev) multitouch_close_device(x, x->dev);
	if (n) {
		CFArrayRef devlist = MTDeviceCreateList();
		CFIndex cnt = CFArrayGetCount(devlist);
		post( "fingerpinger: seen %d multitouch devices \n", cnt);
		if (cnt >= 1) {
			--n;
			if (n >= cnt) { n = cnt - 1; }
            post( "fingerpinger: opened multitouch device id %d\n", n);
			multitouch_open_device(x, (MTDeviceRef)CFArrayGetValueAtIndex(devlist,n));
		}
		CFRelease(devlist);
	}
}



//--------------------------------------------------------------------------
// - Object creation and setup
//--------------------------------------------------------------------------

int C74_EXPORT main(void){
	critical_new(&crit);
    
    
    t_class *c;
    
    c = class_new("fingerpinger", (method)multitouch_new, (method)multitouch_free, (long)sizeof(t_multitouch),
                  0L /* leave NULL!! */, A_DEFSYM, 0);
    
    class_addmethod(c, (method)multitouch_int,			"int",		A_LONG, 0);
    class_register(CLASS_BOX, c);
    s_multitouch_class = c;

    
    post("fingerpinger version 20170812 - (cc) 2017 [ a n y m a ]",0);	// post any important info to the max window when our object is loaded
return 1;
}

//--------------------------------------------------------------------------
// - Object creation
//--------------------------------------------------------------------------

void *multitouch_new(t_symbol *s)
{
    t_multitouch *x ;
    
    x = (t_multitouch *)object_alloc(s_multitouch_class);

	x->out_bang = listout(x);
	x->out_list = bangout(x);
	x->dev = NULL;
	x->evt_qelem = qelem_new(x, (method)multitouch_output);
	x->evts = new FingerQueue;

	return x;													// return a reference to the object instance 
}



//--------------------------------------------------------------------------
// - Object destruction
//--------------------------------------------------------------------------

static void multitouch_free(t_multitouch *x)
{
	multitouch_int(x, 0);										// make sure callback is released before deleting the object
	delete x->evts;
	qelem_free(x->evt_qelem);
}




