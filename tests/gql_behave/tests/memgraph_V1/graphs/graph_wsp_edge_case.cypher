CREATE (a:Start {id: 1}),
       (b:Node {id: 2}),
       (c:Node {id: 3}),
       (d:Node {id: 4}),
       (e:Node {id: 5}),
       (f:Node {id: 6}),
       (a)-[:CONNECTS {cost: 1}]->(b),
       (b)-[:CONNECTS {cost: 1}]->(c),
       (c)-[:CONNECTS {cost: 1}]->(d),
       (d)-[:CONNECTS {cost: 1}]->(f),
       (b)-[:CONNECTS {cost: 5}]->(e),
       (e)-[:CONNECTS {cost: 5}]->(f);
