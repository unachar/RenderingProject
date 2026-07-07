#pragma once

class EntityBase
{
public:
    virtual ~EntityBase() = default;

    virtual void Create() = 0;
};