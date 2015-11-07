#ifndef MEMGRAPH_API_RESOURCES_RELATIONSHIP_HPP
#define MEMGRAPH_API_RESOURCES_RELATIONSHIP_HPP

#include "api/restful/resource.hpp"

#pragma url /relationship
class Relationships : public Resource<Relationships, POST>
{
public:
    using Resource::Resource;

    void post(sp::Request& req, sp::Response& res)
    {
        return res.send("POST /db/data/relationship");
    }

};

#pragma url /relationship/{id:\\d+}
class Relationship : public Resource<Relationship, GET>
{
public:
    using Resource::Resource;

    void get(sp::Request& req, sp::Response& res)
    {
        return res.send("GET /db/data/relationship");
    }
};

#endif
