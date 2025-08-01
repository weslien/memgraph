CREATE (a:Student {name: 'Ana'});
CREATE (b:Student {name: 'Bob'});
CREATE (c:Student {name: 'Carol'});
CREATE (d:Student {name: 'Dave'});
CREATE (e:Student {name: 'Eve'});
MATCH (a:Student {name: 'Ana'}), (b:Student {name: 'Bob'}) CREATE (a)-[:FRIEND]->(b);
MATCH (a:Student {name: 'Ana'}), (c:Student {name: 'Carol'}) CREATE (a)-[:FRIEND]->(c);
MATCH (b:Student {name: 'Bob'}), (d:Student {name: 'Dave'}) CREATE (b)-[:COLLEAGUE]->(d);
MATCH (c:Student {name: 'Carol'}), (e:Student {name: 'Eve'}) CREATE (c)-[:FRIEND]->(e);
MATCH (d:Student {name: 'Dave'}), (e:Student {name: 'Eve'}) CREATE (d)-[:FRIEND]->(e);
