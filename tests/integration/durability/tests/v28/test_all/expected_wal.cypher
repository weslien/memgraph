CREATE ENUM Status VALUES { ACTIVE, DISABLED };
CREATE (:__mg_vertex__:`label2` {__mg_id__: 0, `prop2`: ["kaj", 2, Null, {`prop4`: -1.341}], `prop`: "joj", `ext`: 2});
CREATE (:__mg_vertex__:`label`:`label2` {__mg_id__: 1, `prop`: "joj", `ext`: 2});
CREATE (:__mg_vertex__:`label2` {__mg_id__: 2, `prop2`: 2, `prop`: 1, `status`: Status::ACTIVE});
CREATE (:__mg_vertex__:`label2` {__mg_id__: 3, `prop2`: 2, `prop`: 2, `status`: Status::DISABLED});
CREATE (:__mg_vertex__:`has_coord` {__mg_id__: 4, `coord`: POINT({ x:-73.93, y:40.73, srid: 4326 })});
CREATE (:__mg_vertex__:`has_coord` {__mg_id__: 5, `coord`: POINT({ x:-73.93, y:40.73, z:10, srid: 4979 })});
CREATE (:__mg_vertex__:`has_coord` {__mg_id__: 6, `coord`: POINT({ x:0, y:1, srid: 7203 })});
CREATE (:__mg_vertex__:`has_coord` {__mg_id__: 7, `coord`: POINT({ x:0, y:1, z:2, srid: 9157 })});
CREATE (:__mg_vertex__:`edge_index_from` {__mg_id__: 8});
CREATE (:__mg_vertex__:`edge_index_to` {__mg_id__: 9});
CREATE (:__mg_vertex__:`Label` {__mg_id__: 10, `embedding`: [1, 2, 3]});
CREATE (:__mg_vertex__:`composite` {__mg_id__: 11, `a`: 1, `b`: 2, `c`: 3, `d`: 4});
CREATE (:__mg_vertex__:`composite` {__mg_id__: 12, `a`: 4, `b`: 3, `c`: 2, `d`: 1});
CREATE (:__mg_vertex__:`TTL` {__mg_id__: 13, `ttl`: 4102444800});
CREATE (:__mg_vertex__:`nested` {__mg_id__: 14, `a`: {`b`: {`c`: 1}}});
CREATE (:__mg_vertex__ {__mg_id__: 15});
CREATE (:__mg_vertex__ {__mg_id__: 16});
CREATE INDEX ON :__mg_vertex__(__mg_id__);
MATCH (u:__mg_vertex__), (v:__mg_vertex__) WHERE u.__mg_id__ = 1 AND v.__mg_id__ = 0 CREATE (u)-[:`link` {`prop`: -1, `ext`: [false, {`k`: "l"}]}]->(v);
MATCH (u:__mg_vertex__), (v:__mg_vertex__) WHERE u.__mg_id__ = 1 AND v.__mg_id__ = 1 CREATE (u)-[:`link` {`prop`: -1, `ext`: [false, {`k`: "l"}]}]->(v);
MATCH (u:__mg_vertex__), (v:__mg_vertex__) WHERE u.__mg_id__ = 1 AND v.__mg_id__ = 2 CREATE (u)-[:`link` {`prop`: -1, `ext`: [false, {`k`: "l"}]}]->(v);
MATCH (u:__mg_vertex__), (v:__mg_vertex__) WHERE u.__mg_id__ = 1 AND v.__mg_id__ = 3 CREATE (u)-[:`link` {`prop`: -1, `ext`: [false, {`k`: "l"}]}]->(v);
MATCH (u:__mg_vertex__), (v:__mg_vertex__) WHERE u.__mg_id__ = 8 AND v.__mg_id__ = 9 CREATE (u)-[:`edge_type`]->(v);
MATCH (u:__mg_vertex__), (v:__mg_vertex__) WHERE u.__mg_id__ = 8 AND v.__mg_id__ = 9 CREATE (u)-[:`edge_type` {`prop`: 1}]->(v);
MATCH (u:__mg_vertex__), (v:__mg_vertex__) WHERE u.__mg_id__ = 8 AND v.__mg_id__ = 9 CREATE (u)-[:`edge_type` {`ttl`: 4102444800}]->(v);
MATCH (u:__mg_vertex__), (v:__mg_vertex__) WHERE u.__mg_id__ = 15 AND v.__mg_id__ = 16 CREATE (u)-[:`REL` {`embedding`: [1, 2, 3]}]->(v);
DROP INDEX ON :__mg_vertex__(__mg_id__);
MATCH (u) REMOVE u:__mg_vertex__, u.__mg_id__;
CREATE INDEX ON :`label`;
CREATE INDEX ON :`label2`(`prop2`);
CREATE INDEX ON :`label2`(`prop`);
CREATE INDEX ON :`composite`(`a`, `b`, `c`, `d`);
CREATE INDEX ON :`composite`(`d`, `c`, `b`, `a`);
CREATE INDEX ON :`nested`(`a`.`b`.`c`);
CREATE INDEX ON :`TTL`(`ttl`);
CREATE POINT INDEX ON :`has_coord`(`coord`);
CREATE VECTOR INDEX `vector_index_name` ON :`Label`(`embedding`) WITH CONFIG { "dimension": 3, "metric": "l2sq", "capacity": 1000, "resize_coefficient": 2, "scalar_kind": "f32" };
CREATE VECTOR INDEX `vector_index_name_1` ON :`Label1`(`embedding1`) WITH CONFIG { "dimension": 3, "metric": "l2sq", "capacity": 1000, "resize_coefficient": 2, "scalar_kind": "f16" };
CREATE VECTOR EDGE INDEX `vector_edge_index_name` ON :`REL`(`embedding`) WITH CONFIG { "dimension": 3, "metric": "l2sq", "capacity": 1000, "resize_coefficient": 2, "scalar_kind": "f16" };
CREATE CONSTRAINT ON (u:`label`) ASSERT EXISTS (u.`ext`);
CREATE CONSTRAINT ON (u:`label2`) ASSERT u.`prop2`, u.`prop` IS UNIQUE;
CREATE CONSTRAINT ON (u:`label`) ASSERT u.`is_string` IS TYPED STRING;
CREATE CONSTRAINT ON (u:`label`) ASSERT u.`is_int` IS TYPED INTEGER;
CREATE CONSTRAINT ON (u:`label`) ASSERT u.`is_list` IS TYPED LIST;
CREATE CONSTRAINT ON (u:`label`) ASSERT u.`is_enum` IS TYPED ENUM;
CREATE CONSTRAINT ON (u:`label`) ASSERT u.`is_point` IS TYPED POINT;
CREATE EDGE INDEX ON :`edge_type`;
CREATE EDGE INDEX ON :`edge_type`(`prop`);
CREATE GLOBAL EDGE INDEX ON :(`ttl`);
