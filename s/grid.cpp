// grid.cpp

/**
*    Copyright (C) 2010 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"

#include <iomanip>

#include "../client/connpool.h"

#include "grid.h"
#include "shard.h"

namespace mongo {
    
    DBConfigPtr Grid::getDBConfig( string database , bool create ){
        {
            string::size_type i = database.find( "." );
            if ( i != string::npos )
                database = database.substr( 0 , i );
        }
        
        if ( database == "config" )
            return configServerPtr;

        scoped_lock l( _lock );

        DBConfigPtr& cc = _databases[database];
        if ( !cc ){
            cc.reset(new DBConfig( database ));
            if ( ! cc->load() ){
                if ( create ){
                    // note here that cc->primary == 0.
                    log() << "couldn't find database [" << database << "] in config db" << endl;
                    
                    { // lets check case
                        ScopedDbConnection conn( configServer.modelServer() );
                        BSONObjBuilder b;
                        b.appendRegex( "_id" , (string)"^" + database + "$" , "i" );
                        BSONObj d = conn->findOne( ShardNS::database , b.obj() );
                        conn.done();

                        if ( ! d.isEmpty() ){
                            cc.reset();
                            stringstream ss;
                            ss <<  "can't have 2 databases that just differ on case " 
                               << " have: " << d["_id"].String()
                               << " want to add: " << database;

                            uasserted( DatabaseDifferCaseCode ,ss.str() );
                        }
                    }

                    Shard primary;
                    if ( database == "admin" )
                        primary = configServer.getPrimary();
                    else
                        primary = Shard::pick();

                    if ( primary.ok() ){
                        cc->setPrimary( primary.getName() ); // saves 'cc' to configDB
                        log() << "\t put [" << database << "] on: " << primary << endl;
                    }
                    else {
                        cc.reset();
                        log() << "\t can't find a shard to put new db on" << endl;
                        uasserted( 10185 ,  "can't find a shard to put new db on" );
                    }
                }
                else {
                    cc.reset();
                }
            }
            
        }
        
        return cc;
    }

    void Grid::removeDB( string database ){
        uassert( 10186 ,  "removeDB expects db name" , database.find( '.' ) == string::npos );
        scoped_lock l( _lock );
        _databases.erase( database );
        
    }

    bool Grid::allowLocalHost() const {
        return _allowLocalShard;
    }

    void Grid::setAllowLocalHost( bool allow ){
        _allowLocalShard = allow;
    }

    bool Grid::addShard( string* name , const string& host , long long maxSize , string* errMsg ){
        // errMsg is required but name is optional
        DEV assert( errMsg );
        string nameInternal;
        if ( ! name ) {
            name = &nameInternal;
        }

        // check whether host exists and is operative
        try {
            ScopedDbConnection newShardConn( host );
            newShardConn->getLastError();
            newShardConn.done();
        }
        catch ( DBException& e ){
            ostringstream ss;
            ss << "couldn't connect to new shard ";
            ss << e.what();
            *errMsg = ss.str();
            return false;
        }
                
        // if a name for a shard wasn't provided, pick one.
        if ( name->empty() && ! _getNewShardName( name ) ){
            *errMsg = "error generating new shard name";
            return false;
        }
            
        // build the ConfigDB shard document.
        BSONObjBuilder b;
        b.append( "_id" , *name );
        b.append( "host" , host );
        if ( maxSize > 0 ){
            b.append( ShardFields::maxSize.name() , maxSize );
        }
        BSONObj shardDoc = b.obj();

        {
            ScopedDbConnection conn( configServer.getPrimary() );
                
            // check whether this host:port is already a known shard
            BSONObj old = conn->findOne( ShardNS::shard , BSON( "host" << host ) );
            if ( ! old.isEmpty() ){
                *errMsg = "host already used";
                conn.done();
                return false;
            }

            log() << "going to add shard: " << shardDoc << endl;

            conn->insert( ShardNS::shard , shardDoc );
            *errMsg = conn->getLastError();
            if ( ! errMsg->empty() ){
                log() << "error adding shard: " << shardDoc << " err: " << *errMsg << endl;
                return false;
            }

            conn.done();
        }

        Shard::reloadShardInfo();
        return true;
    }
        
    bool Grid::knowAboutShard( const string& name ) const{
        ShardConnection conn( configServer.getPrimary() , "" );
        BSONObj shard = conn->findOne( ShardNS::shard , BSON( "host" << name ) );
        conn.done();
        return ! shard.isEmpty();
    }

    bool Grid::_getNewShardName( string* name ) const{
        DEV assert( name );

        bool ok = false;
        int count = 0; 

        ShardConnection conn( configServer.getPrimary() , "" );
        BSONObj o = conn->findOne( ShardNS::shard , Query( fromjson ( "{_id: /^shard/}" ) ).sort(  BSON( "_id" << -1 ) ) ); 
        if ( ! o.isEmpty() ) {
            string last = o["_id"].String();
            istringstream is( last.substr( 5 ) );
            is >> count;
            count++;
        }                                                                                                               
        if (count < 9999) {
            stringstream ss;
            ss << "shard" << setfill('0') << setw(4) << count;
            *name = ss.str();
            ok = true;
        }
        conn.done();

        return ok;
    }

    bool Grid::shouldBalance() const {
        ShardConnection conn( configServer.getPrimary() , "" );

        // look for the stop balancer marker
        BSONObj stopMarker = conn->findOne( ShardNS::settings, BSON( "_id" << "balancer" << "stopped" << true ) );
        conn.done();
        return stopMarker.isEmpty();
    }

    unsigned long long Grid::getNextOpTime() const {
        ScopedDbConnection conn( configServer.getPrimary() );
        
        BSONObj result;
        massert( 10421 ,  "getoptime failed" , conn->simpleCommand( "admin" , &result , "getoptime" ) );
        conn.done();

        return result["optime"]._numberLong();
    }

    Grid grid;

} 
