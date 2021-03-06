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
#include "health.h"
#include "../../util/background.h"
#include "../../client/dbclient.h"
#include "../commands.h"
#include "../../util/concurrency/value.h"
#include "../../util/concurrency/task.h"
#include "../../util/mongoutils/html.h"
#include "../../util/goodies.h"
#include "../../util/ramlog.h"
#include "../helpers/dblogger.h"
#include "connections.h"

namespace mongo {
    /* decls for connections.h */
    ScopedConn::M& ScopedConn::_map = *(new ScopedConn::M());    
    mutex ScopedConn::mapMutex("ScopedConn::mapMutex");
}

namespace mongo { 

    using namespace mongoutils::html;
    using namespace bson;

    static RamLog _rsLog;
    Tee *rsLog = &_rsLog;

    string ago(time_t t) { 
        if( t == 0 ) return "";

        time_t x = time(0) - t;
        stringstream s;
        if( x < 180 ) {
            s << x << " sec";
            if( x != 1 ) s << 's';
        }
        else if( x < 3600 ) {
            s.precision(2);
            s << x / 60.0 << " mins";
        }
        else { 
            s.precision(2);
            s << x / 3600.0 << " hrs";
        }
        return s.str();
    }

    void Member::summarizeAsHtml(stringstream& s) const { 
        s << tr();
        {
            stringstream u;
            u << "http://" << h().host() << ':' << (h().port() + 1000) << "/_replSet";
            s << td( a(u.str(), "", fullName()) );
        }
        s << td( id() );
        double h = hbinfo().health;
        bool ok = h > 0;
        s << td(red(str::stream() << h,h == 0));
        s << td(ago(hbinfo().upSince));
        bool never = false;
        {
            string h;
            time_t hb = hbinfo().lastHeartbeat;
            if( hb == 0 ) {
                h = "never";
                never = true;
            }
            else h = ago(hb) + " ago";
            s << td(h);
        }
        s << td(config().votes);
        { 
            string stateText = ReplSet::stateAsStr(state());
            if( ok || stateText.empty() ) 
                s << td(stateText); // text blank if we've never connected
            else
                s << td( grey(str::stream() << "(was " << ReplSet::stateAsStr(state()) << ')', true) );
        }
        s << td( grey(hbinfo().lastHeartbeatMsg,!ok) );
        stringstream q;
        q << "/_replSetOplog?" << id();
        s << td( a(q.str(), "", never ? "?" : hbinfo().opTime.toString()) );
        s << _tr();
    }
    
    string ReplSetImpl::stateAsHtml(MemberState s) { 
        if( s.s == MemberState::RS_STARTUP ) return a("", "serving still starting up, or still trying to initiate the set", "STARTUP");
        if( s.s == MemberState::RS_PRIMARY ) return a("", "this server thinks it is primary", "PRIMARY");
        if( s.s == MemberState::RS_SECONDARY ) return a("", "this server thinks it is a secondary (slave mode)", "SECONDARY");
        if( s.s == MemberState::RS_RECOVERING ) return a("", "recovering/resyncing; after recovery usually auto-transitions to secondary", "RECOVERING");
        if( s.s == MemberState::RS_FATAL ) return a("", "something bad has occurred and server is not completely offline with regard to the replica set.  fatal error.", "RS_FATAL");
        if( s.s == MemberState::RS_STARTUP2 ) return a("", "loaded config, still determining who is primary", "RS_STARTUP2");
        if( s.s == MemberState::RS_ARBITER ) return a("", "this server is an arbiter only", "ARBITER");
        if( s.s == MemberState::RS_DOWN ) return a("", "member is down, slow, or unreachable", "DOWN");
        return "";
    }

    string ReplSetImpl::stateAsStr(MemberState s) { 
        if( s.s == MemberState::RS_STARTUP ) return "STARTUP";
        if( s.s == MemberState::RS_PRIMARY ) return "PRIMARY";
        if( s.s == MemberState::RS_SECONDARY ) return "SECONDARY";
        if( s.s == MemberState::RS_RECOVERING ) return "RECOVERING";
        if( s.s == MemberState::RS_FATAL ) return "FATAL";
        if( s.s == MemberState::RS_STARTUP2 ) return "STARTUP2";
        if( s.s == MemberState::RS_ARBITER ) return "ARBITER";
        if( s.s == MemberState::RS_DOWN ) return "DOWN";
        return "";
    }

    extern time_t started;

    // oplogdiags in web ui
    static void say(stringstream&ss, const bo& op) {
        ss << "<tr>";

        set<string> skip;
        be e = op["ts"];
        if( e.type() == Date || e.type() == Timestamp ) { 
            OpTime ot = e._opTime();
            ss << td( a("",ot.toString(),ot.toStringPretty()) );
            skip.insert("ts");
        }
        else ss << td("?");

        e = op["h"];
        if( e.type() == NumberLong ) {
            ss << "<td>" << hex << e.Long() << "</td>\n";
            skip.insert("h");
        } else
            ss << td("?");

        ss << td(op["op"].valuestrsafe());
        ss << td(op["ns"].valuestrsafe());
        skip.insert("op");
        skip.insert("ns");

        ss << "<td>";
        for( bo::iterator i(op); i.more(); ) { 
            be e = i.next();
            if( skip.count(e.fieldName()) ) continue;
            ss << e.toString() << ' ';
        }
        ss << "</td>";
        ss << "</tr>";
        ss << '\n';
    }

    void ReplSetImpl::_getOplogDiagsAsHtml(unsigned server_id, stringstream& ss) const { 
        Member *m = findById(server_id);
        if( m == 0 ) { 
            ss << "Error : can't find a member with id: " << server_id << '\n';
            return;
        }

        ss << p("Server : " + m->fullName() + "<br>ns : " + rsoplog );

        //const bo fields = BSON( "o" << false << "o2" << false );
        const bo fields;

        ScopedConn conn(m->fullName());        

        auto_ptr<DBClientCursor> c = conn->query(rsoplog, Query().sort("$natural",1), 20, 0, &fields);
        static const char *h[] = {"ts","h","op","ns","rest",0};

        ss << "<style type=\"text/css\" media=\"screen\">"
            "table { font-size:75% }\n"
//            "th { background-color:#bbb; color:#000 }\n"
//            "td,th { padding:.25em }\n"
            "</style>\n";
        
        ss << table(h, true);
        //ss << "<pre>\n";
        int n = 0;
        OpTime otFirst;
        OpTime otLast;
        OpTime otEnd;
        while( c->more() ) {
            bo o = c->next();
            otLast = o["ts"]._opTime();
            if( otFirst.isNull() ) 
                otFirst = otLast;
            say(ss, o);
            n++;            
        }
        if( n == 0 ) {
            ss << rsoplog << " is empty\n";
        }
        else { 
            auto_ptr<DBClientCursor> c = conn->query(rsoplog, Query().sort("$natural",-1), 20, 0, &fields);
            string x;
            bo o = c->next();
            otEnd = o["ts"]._opTime();
            while( 1 ) {
                stringstream z;
                if( o["ts"]._opTime() == otLast ) 
                    break;
                say(z, o);
                x = z.str() + x;
                if( !c->more() )
                    break;
                bo o = c->next();
            }
            if( !x.empty() ) {
                ss << "<tr><td>...</td><td>...</td><td>...</td><td>...</td><td>...</td></tr>\n" << x;
                //ss << "\n...\n\n" << x;
            }
        }
        ss << _table();
        //ss << "</pre>\n";

        if( !otEnd.isNull() ) {
            ss << "<p>Log length in time: ";
            unsigned d = otEnd.getSecs() - otFirst.getSecs();
            double h = d / 3600.0;
            ss.precision(3);
            if( h < 72 )
                ss << h << " hours";
            else 
                ss << h / 24.0 << " days";
            ss << "</p>\n";
        }
        ss << p("Current time: " + time_t_to_String_short(time(0)));
    }

    void ReplSetImpl::_summarizeAsHtml(stringstream& s) const { 
        s << table(0, false);
        s << tr("Set name:", _name);
        s << tr("Majority up:", elect.aMajoritySeemsToBeUp()?"yes":"no" );
        s << _table();

        const char *h[] = {"Member", 
            "<a title=\"member id in the replset config\">id</a>", 
            "Up", 
            "<a title=\"length of time we have been continuously connected to the other member with no reconnects (for self, shows uptime)\">cctime</a>", 
            "<a title=\"when this server last received a heartbeat response - includes error code responses\">Last heartbeat</a>", 
            "Votes", "State", "Status", 
            "<a title=\"how up to date this server is; write operations are sequentially numbered.  this value polled every few seconds so actually lag is typically much lower than value shown here.\">opord</a>", 
            0};
        s << table(h);

        /* this is to sort the member rows by their ordinal _id, so they show up in the same 
           order on all the different web ui's; that is less confusing for the operator. */
        map<int,string> mp;

        {
            stringstream s;
            /* self row */
            s << tr() << td(_self->fullName() + " (me)") <<
                td(_self->id()) <<
  	        td("1") <<  //up
                td(ago(started)) << 
	        td("") << // last heartbeat
                td(ToString(_self->config().votes)) << 
                td(stateAsHtml(box.getState()));
            s << td( _hbmsg );
            stringstream q;
            q << "/_replSetOplog?" << _self->id();
            s << td( a(q.str(), "", theReplSet->lastOpTimeWritten.toString()) );
            s << _tr();
			mp[_self->hbinfo().id()] = s.str();
        }
        Member *m = head();
        while( m ) {
			stringstream s;
            m->summarizeAsHtml(s);
			mp[m->hbinfo().id()] = s.str();
            m = m->next();
        }

        for( map<int,string>::const_iterator i = mp.begin(); i != mp.end(); i++ )
            s << i->second;
        s << _table();
    }


    void fillRsLog(stringstream& s) {
        _rsLog.toHTML( s );
    }

    Member* ReplSetImpl::findById(unsigned id) const { 
        if( id == _self->id() ) return _self;
        for( Member *m = head(); m; m = m->next() )
            if( m->id() == id ) 
                return m;
        return 0;
    }

    void ReplSetImpl::_summarizeStatus(BSONObjBuilder& b) const { 
        Member *m =_members.head();
        vector<BSONObj> v;

        // add self
        {
            HostAndPort h(getHostName(), cmdLine.port);
            v.push_back( 
                BSON( "name" << h.toString() << "self" << true << 
                      "errmsg" << _self->lhb() ) );
        }

        while( m ) {
            BSONObjBuilder bb;
            bb.append("name", m->fullName());
            bb.append("health", m->hbinfo().health);
            bb.append("uptime", (unsigned) (m->hbinfo().upSince ? (time(0)-m->hbinfo().upSince) : 0));
            bb.appendTimeT("lastHeartbeat", m->hbinfo().lastHeartbeat);
            bb.append("errmsg", m->lhb());
            v.push_back(bb.obj());
            m = m->next();
        }
        b.append("set", name());
        b.appendTimeT("date", time(0));
        b.append("myState", box.getState().s);
        b.append("members", v);
    }

}
