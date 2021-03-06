/* @file manager.cpp 
*/

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,b
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "rs.h"
#include "../client.h"

namespace mongo {

    enum { 
        NOPRIMARY = -2,
        SELFPRIMARY = -1
    };

    /* check members OTHER THAN US to see if they think they are primary */
    const Member * Manager::findOtherPrimary() { 
        Member *m = rs->head();
        Member *p = 0;
        while( m ) {
            if( m->state().primary() && m->hbinfo().up() ) {
                if( p ) throw "twomasters"; // our polling is asynchronous, so this is often ok.
                p = m;
            }
            m = m->next();
        }
        if( p ) 
            noteARemoteIsPrimary(p);
        return p;
    }

    Manager::Manager(ReplSetImpl *_rs) : 
    task::Server("rs Manager"), rs(_rs), busyWithElectSelf(false), _primary(NOPRIMARY)
    { 
    }

    Manager::~Manager() { 
        log() << "should never be called?" << rsLog;
        rs->mgr = 0;
        assert(false);
    }

    void Manager::starting() { 
        Client::initThread("rs Manager");
    }

    void Manager::noteARemoteIsPrimary(const Member *m) { 
        if( rs->box.getPrimary() == m )
            return;
        rs->_self->lhb() = "";
        rs->box.set(rs->iAmArbiterOnly() ? MemberState::RS_ARBITER : MemberState::RS_RECOVERING, m);
    }

    /** called as the health threads get new results */
    void Manager::msgCheckNewState() {
        {
            theReplSet->assertValid();
            rs->assertValid();

            RSBase::lock lk(rs);

            if( busyWithElectSelf ) return;

            const Member *p = rs->box.getPrimary();
            if( p && p != rs->_self ) {
                if( !p->hbinfo().up() || 
                    !p->hbinfo().hbstate.primary() ) 
                {
                    p = 0;
                    rs->box.setOtherPrimary(0);
                }
            }

            const Member *p2;
            try { p2 = findOtherPrimary(); }
            catch(string s) { 
                /* two other nodes think they are primary (asynchronously polled) -- wait for things to settle down. */
                log() << "replSet warning DIAG 2 primary" << s << rsLog;
                return;
            }

            if( p2 ) {
                /* someone else thinks they are primary. */
                if( p == p2 ) { 
                    // we thought the same; all set.
                    return;
                }
                if( p == 0 ) {
                    noteARemoteIsPrimary(p2); 
                    return;
                }
                // todo xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
                if( p != rs->_self ) {
                    // switch primary from oldremotep->newremotep2
                    noteARemoteIsPrimary(p2); 
                    return;
                }
                /* we thought we were primary, yet now someone else thinks they are. */
                if( !rs->elect.aMajoritySeemsToBeUp() ) {
                    /* we can't see a majority.  so the other node is probably the right choice. */
                    noteARemoteIsPrimary(p2); 
                    return;
                }
                /* ignore for now, keep thinking we are master. 
                   this could just be timing (we poll every couple seconds) or could indicate 
                   a problem?  if it happens consistently for a duration of time we should 
                   alert the sysadmin.
                */
                return;
            }

            /* didn't find anyone who wants to be primary */

            if( p ) { 
                /* we are already primary */

                if( p != rs->_self ) { 
                    rs->sethbmsg("error p != rs->self in checkNewState");
                    log() << "replSet " << p->fullName() << rsLog;
                    log() << "replSet " << rs->_self->fullName() << rsLog;
                    return;
                }

                if( !rs->elect.aMajoritySeemsToBeUp() ) { 
                    log() << "replSet can't see a majority of the set, relinquishing primary" << rsLog;
                    rs->relinquish();
                }

                return;
            }

            if( !rs->iAmPotentiallyHot() ) // if not we never try to be primary
                return;

            /* TODO : CHECK PRIORITY HERE.  can't be elected if priority zero. */

            /* no one seems to be primary.  shall we try to elect ourself? */
            if( !rs->elect.aMajoritySeemsToBeUp() ) { 
                static int n;
                log(++n <= 5 ? 0 : 1) << "replSet can't see a majority, won't consider electing self" << rsLog;
                return;
            }

            rs->sethbmsg("",9);
            busyWithElectSelf = true; // don't try to do further elections & such while we are already working on one.
        }
        try { 
            rs->elect.electSelf(); 
        }
        catch(RetryAfterSleepException&) {
            /* we want to process new inbounds before trying this again.  so we just put a checkNewstate in the queue for eval later. */
            requeue();
        }
        catch(...) { 
            log() << "replSet error unexpected assertion in rs manager" << rsLog; 
        }
        busyWithElectSelf = false;
    }

}
