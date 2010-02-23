t = db.jstests_not2;
t.drop();

check = function( query, expected, size ) {
    if ( size == null ) {
        size = 1;
    }
    assert.eq( size, t.count( query ), tojson( query ) );
    if ( size > 0 ) {
        assert.eq( expected, t.findOne( query ).i, tojson( query ) );
    }
}

fail = function( query ) {
    try {
        t.count( query );
    } catch ( e ) {
    }
    assert( db.getLastError(), tojson( query ) );
}

t.save( {i:"a"} );
t.save( {i:"b"} );

fail( {i:{$not:"a"}} );
fail( {i:{$not:{$not:{$gt:"a"}}}} );
fail( {i:{$not:{$ref:"foo"}}} );
fail( {i:{$not:{}}} );
check( {i:{$gt:"a"}}, "b" );
check( {i:{$not:{$gt:"a"}}}, "a" );
check( {i:{$not:{$ne:"a"}}}, "a" );
check( {i:{$not:{$gte:"b"}}}, "a" );
check( {i:{$exists:true}}, "a", 2 );
check( {i:{$not:{$exists:true}}}, "", 0 );
check( {j:{$not:{$exists:false}}}, "", 0 );
check( {j:{$not:{$exists:true}}}, "a", 2 );
check( {i:{$not:{$in:["a"]}}}, "b" );
check( {i:{$not:{$in:["a", "b"]}}}, "", 0 );
check( {i:{$not:{$in:["g"]}}}, "a", 2 );
check( {i:{$not:{$nin:["a"]}}}, "a" );
check( {i:{$not:/a/}}, "b" );
check( {i:{$not:/(a|b)/}}, "", 0 );
check( {i:{$not:/a/,$regex:"a"}}, "", 0 );
check( {i:{$not:/aa/}}, "a", 2 );
fail( {i:{$not:{$regex:"a"}}} );
fail( {i:{$not:{$options:"a"}}} );
check( {i:{$type:2}}, "a", 2 );
check( {i:{$not:{$type:1}}}, "a", 2 );
check( {i:{$not:{$type:2}}}, "", 0 );

check( {i:{$not:{$gt:"c",$lt:"b"}}}, "b" );

t.drop();
t.save( {i:1} );
check( {i:{$not:{$mod:[5,1]}}}, null, 0 );
check( {i:{$mod:[5,2]}}, null, 0 );
check( {i:{$not:{$mod:[5,2]}}}, 1, 1 );

t.drop();
t.save( {i:["a","b"]} );
check( {i:{$not:{$size:2}}}, null, 0 );
check( {i:{$not:{$size:3}}}, ["a","b"] );
check( {i:{$not:{$gt:"a"}}}, null, 0 );
check( {i:{$not:{$gt:"c"}}}, ["a","b"] );
check( {i:{$not:{$all:["a","b"]}}}, null, 0 );
check( {i:{$not:{$all:["c"]}}}, ["a","b"] );

t.drop();
t.save( {i:{j:"a"}} );
t.save( {i:{j:"b"}} );
check( {i:{$not:{$elemMatch:{j:"a"}}}}, {j:"b"} );
check( {i:{$not:{$elemMatch:{j:"f"}}}}, {j:"a"}, 2 );

t.drop();
t.save( {i:"a"} );
t.save( {i:"b"} );
t.ensureIndex( {i:1} );

indexed = function( query, min, max ) {
    min = min || MinKey;
    max = max || MaxKey;
    exp = t.find( query ).explain();
    printjson( exp );
    assert( exp.cursor.match( /Btree/ ), tojson( query ) );    
    assert( exp.allPlans.length == 1, tojson( query ) );    
    assert.eq( exp.startKey.i, min );
    assert.eq( exp.endKey.i, max );
}

not = function( query ) {
    exp = t.find( query ).explain();
    printjson( exp );
    assert( !exp.cursor.match( /Btree/ ), tojson( query ) );    
    assert( exp.allPlans.length == 1, tojson( query ) );    
}

indexed( {i:1}, 1, 1 );
not( {i:{$ne:1}} );

indexed( {i:{$not:{$ne:"a"}}}, "a", "a" );
not( {i:{$not:/^a/}} );

//indexed( {i:{$gt:"a"}}, "a", null );
//indexed( {i:{$not:{$gt:"a"}}}, null, "a" );
//
//indexed( {i:{$gte:"a"}}, "a", null );
//indexed( {i:{$not:{$gte:"a"}}}, null, "a" );
//
//indexed( {i:{$lt:"b"}}, null, "b" );
//indexed( {i:{$not:{$gte:"b"}}}, "b", null );
//
//indexed( {i:{$lte:"b"}}, null, "b" );
//indexed( {i:{$not:{$gt:"b"}}}, "b", null );