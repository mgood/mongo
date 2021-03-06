// cursor1.js
// checks that cursors survive a chunk's move

s = new ShardingTest( "sharding_cursor1" , 2 , 2 )

// take the balancer out of the equation
s.config.settings.update( { _id: "balancer" }, { $set : { stopped: true } } , true );
s.config.settings.find().forEach( printjson )

// create a sharded 'test.foo', for the moment with just one chunk
s.adminCommand( { enablesharding: "test" } );
s.adminCommand( { shardcollection: "test.foo", key: { _id: 1 } } ) 

db = s.getDB( "test" );
primary = s.getServer( "test" ).getDB( "test" );
secondary = s.getOther( primary ).getDB( "test" );

numObjs = 10;
for (i=0; i < numObjs; i++){                                                                                                  
    db.foo.insert({_id: i}); 
} 
db.getLastError();
assert.eq( 1, s.config.chunks.count() , "test requires collection to have one chunk initially" );

// we'll split the collection in two and move the second chunk while three cursors are open
// cursor1 still has more data in the first chunk, the one that didn't move
// cursor2 buffered the last obj of the first chunk
// cursor3 buffered data that was moved on the second chunk
var cursor1 = db.foo.find().batchSize( 3 );
assert.eq( 3 , cursor1.objsLeftInBatch() );
var cursor2 = db.foo.find().batchSize( 5 );
assert.eq( 5 , cursor2.objsLeftInBatch() );
var cursor3 = db.foo.find().batchSize( 7 );
assert.eq( 7 , cursor3.objsLeftInBatch() );

s.adminCommand( { split: "test.foo" , middle : { _id : 5 } } ); 
s.adminCommand( { movechunk : "test.foo" , find : { _id : 5 } , to : secondary.getMongo().name } );
assert.eq( 2, s.config.chunks.count() );
 
// the cursors should not have been affected
assert.eq( numObjs , cursor1.itcount() , "c1" );
assert.eq( numObjs , cursor2.itcount() , "c2" );
assert.eq( numObjs , cursor3.itcount() , "c3" );

s.stop()
