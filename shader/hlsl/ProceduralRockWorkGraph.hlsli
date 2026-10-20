float3 RockReadPosition(uint index)
{
    return asfloat(Output.Load3(index*44));
}
uint RockMidpoint(uint first,uint second,uint headOffset,uint edgeOffset,inout uint vertexCount,inout uint edgeCount)
{
    uint lower=min(first,second);
    uint upper=max(first,second);
    uint edge=Output.Load(headOffset+lower*4);
    while(edge!=0xffffffffu)
    {
        uint3 entry=Output.Load3(edgeOffset+edge*12);
        if(entry.x==upper)
        {
            return entry.y;
        }
        edge=entry.z;
    }
    uint index=vertexCount++;
    float3 position=normalize((RockReadPosition(first)+RockReadPosition(second))*0.5);
    Output.Store3(index*44,asuint(position));
    uint previous=Output.Load(headOffset+lower*4);
    Output.Store3(edgeOffset+edgeCount*12,uint3(upper,index,previous));
    Output.Store(headOffset+lower*4,edgeCount++);
    return index;
}
void GenerateRock(TextureParams parameters)
{
    uint capacity=Parameters.Load(172);
    uint indexCapacity=Parameters.Load(176);
    uint subdivisions=Parameters.Load(180);
    uint indexOffset=capacity*44;
    uint temporaryOffset=indexOffset+indexCapacity*4;
    uint headOffset=temporaryOffset+indexCapacity*4;
    uint edgeOffset=headOffset+capacity*4;
    float golden=(1+sqrt(5.0))/2;
    float3 initial[12]={float3(-1,golden,0),float3(1,golden,0),float3(-1,-golden,0),float3(1,-golden,0),float3(0,-1,golden),float3(0,1,golden),float3(0,-1,-golden),float3(0,1,-golden),float3(golden,0,-1),float3(golden,0,1),float3(-golden,0,-1),float3(-golden,0,1)};
    uint initialIndices[60]={0,11,5,0,5,1,0,1,7,0,7,10,0,10,11,1,5,9,5,11,4,11,10,2,10,7,6,7,1,8,3,9,4,3,4,2,3,2,6,3,6,8,3,8,9,4,9,5,2,4,11,6,2,10,8,6,7,9,8,1};
    for(uint index=0;index<12;++index)
    {
        Output.Store3(index*44,asuint(normalize(initial[index])));
    }
    for(uint index=0;index<60;++index)
    {
        Output.Store(indexOffset+index*4,initialIndices[index]);
    }
    uint vertexCount=12;
    uint indexCount=60;
    for(uint level=0;level<subdivisions;++level)
    {
        for(uint index=0;index<vertexCount;++index)
        {
            Output.Store(headOffset+index*4,0xffffffffu);
        }
        uint edgeCount=0;
        for(uint index=0;index<indexCount;index+=3)
        {
            uint3 face=Output.Load3(indexOffset+index*4);
            uint a=RockMidpoint(face.x,face.y,headOffset,edgeOffset,vertexCount,edgeCount);
            uint b=RockMidpoint(face.y,face.z,headOffset,edgeOffset,vertexCount,edgeCount);
            uint c=RockMidpoint(face.z,face.x,headOffset,edgeOffset,vertexCount,edgeCount);
            uint children[12]={face.x,a,c,face.y,b,a,face.z,c,b,a,b,c};
            for(uint child=0;child<12;++child)
            {
                Output.Store(temporaryOffset+(index*4+child)*4,children[child]);
            }
        }
        indexCount*=4;
        for(uint index=0;index<indexCount;++index)
        {
            Output.Store(indexOffset+index*4,Output.Load(temporaryOffset+index*4));
        }
    }
    for(uint index=0;index<vertexCount;++index)
    {
        float3 position=RockReadPosition(index);
        float2 uv=float2(0.5+atan2(position.z,position.x)/(2*kPi),0.5-asin(position.y)/kPi);
        float displacement=FractalNoise(parameters.Noise,position.x*0.5+7,position.y*0.5+3,position.z*0.5+5,false);
        position*=1+(displacement-0.5)*2*parameters.Turbulence;
        float gray=0.18+0.22*FractalNoise(parameters.Noise,position.x*2,position.y*2,position.z*2,false);
        WriteModelVertex(index,position,0,float3(gray*1.05,gray,gray*0.92),uv);
    }
    for(uint index=0;index<indexCount;index+=3)
    {
        uint3 face=Output.Load3(indexOffset+index*4);
        float3 normal=cross(RockReadPosition(face.y)-RockReadPosition(face.x),RockReadPosition(face.z)-RockReadPosition(face.x));
        for(uint corner=0;corner<3;++corner)
        {
            uint offset=face[corner]*44+12;
            Output.Store3(offset,asuint(asfloat(Output.Load3(offset))+normal));
        }
    }
    for(uint index=0;index<vertexCount;++index)
    {
        uint offset=index*44+12;
        float3 normal=asfloat(Output.Load3(offset));
        if(length(normal)>1e-12)
        {
            normal=normalize(normal);
        }
        else
        {
            normal=float3(0,1,0);
        }
        Output.Store3(offset,asuint(normal));
    }
}
