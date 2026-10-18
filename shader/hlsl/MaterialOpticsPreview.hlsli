#ifndef MATERIAL_OPTICS_PREVIEW_INCLUDED
#define MATERIAL_OPTICS_PREVIEW_INCLUDED
// Isolated material-editor studio. No scene TLAS or opaque screen copy is used.
// Sphere/cube exits are analytic; the plane is a parallel slab.
float3 MaterialStudioEnvironment(float3 direction, float roughness)
{
    if (SceneRayParameters.x > 0.5f) return SampleEnvironmentLatLong(direction,roughness);
    float3 d=normalize(direction);
    float3 sky=lerp(float3(0.10f,0.12f,0.16f),float3(0.55f,0.62f,0.72f),saturate(d.y*0.5f+0.5f));
    float exponent=lerp(220.0f,3.0f,saturate(roughness)*saturate(roughness));
    float key=pow(saturate(dot(d,normalize(float3(-0.6f,0.7f,-0.5f)))),exponent);
    float fill=pow(saturate(dot(d,normalize(float3(0.8f,0.3f,0.4f)))),exponent*0.6f);
    return sky+(key*float3(8,7.5f,7)+fill*float3(3,3.5f,4))*max(SceneRayParameters.y,0.0f);
}
float3 MaterialStudioBackdrop(float3 origin, float3 direction)
{
    float3 facing=SafeNormalizeCommon(CameraPos,float3(0,0,-1));
    float denominator=dot(direction,facing);
    if (abs(denominator)<1e-5f) return 0.0f;
    float distance=dot(-facing*3.0f-origin,facing)/denominator;
    if (distance<=0) return 0.0f;
    float3 right=SafeNormalizeCommon(cross(float3(0,1,0),facing),float3(1,0,0));
    float3 up=cross(facing,right);
    float3 p=origin+direction*distance;
    float2 uv=float2(dot(p,right),dot(p,up));
    float checker=frac((floor(uv.x*1.5f)+floor(uv.y*1.5f))*0.5f)*2;
    float3 c=lerp(float3(0.08f,0.12f,0.18f),float3(0.8f,0.72f,0.52f),checker);
    return c*(0.5f+0.5f*max(SceneRayParameters.y,0.0f));
}
float3 MaterialStudioTransmission(float3 origin, float3 direction, float roughness)
{
    float3 tangent=SafeNormalizeCommon(cross(direction,float3(0,1,0)),float3(1,0,0));
    float3 bitangent=cross(direction,tangent);
    float radius=roughness*roughness*0.15f;
    float3 c=MaterialStudioBackdrop(origin,direction)*0.5f;
    c+=MaterialStudioBackdrop(origin,normalize(direction+tangent*radius))*0.125f;
    c+=MaterialStudioBackdrop(origin,normalize(direction-tangent*radius))*0.125f;
    c+=MaterialStudioBackdrop(origin,normalize(direction+bitangent*radius))*0.125f;
    c+=MaterialStudioBackdrop(origin,normalize(direction-bitangent*radius))*0.125f;
    return c;
}
bool MaterialPreviewExit(float3 p, float3 direction, uint shape, float thickness,
    out float distance, out float3 outward)
{
    distance=0; outward=0;
    if (shape==1)
    {
        float b=dot(p,direction);
        float discriminant=b*b-dot(p,p)+1.0f;
        if (discriminant<0) return false;
        distance=-b+sqrt(discriminant);
        outward=normalize(p+direction*distance);
    }
    else if (shape==2)
    {
        float3 d=direction;
        d.x=abs(d.x)<1e-6f ? (d.x<0 ? -1e-6f : 1e-6f) : d.x;
        d.y=abs(d.y)<1e-6f ? (d.y<0 ? -1e-6f : 1e-6f) : d.y;
        d.z=abs(d.z)<1e-6f ? (d.z<0 ? -1e-6f : 1e-6f) : d.z;
        float3 farPlane=(sign(d)-p)/d;
        distance=min(farPlane.x,min(farPlane.y,farPlane.z));
        if (farPlane.x<=farPlane.y && farPlane.x<=farPlane.z) outward=float3(sign(d.x),0,0);
        else if (farPlane.y<=farPlane.z) outward=float3(0,sign(d.y),0);
        else outward=float3(0,0,sign(d.z));
    }
    else
    {
        outward=float3(0,0,1);
        if (dot(direction,outward)<0) outward=-outward;
        distance=max(thickness,0.0f)/max(abs(dot(direction,outward)),1e-4f);
    }
    return distance>=0;
}
// A single analytic direct studio light, not environment-map illumination.
float3 MaterialStudioDirect(SurfaceData surface, float3 N, float3 V)
{
    const float PI=3.14159265359f;
    float3 L=normalize(float3(-0.5f,0.7f,-0.8f));
    float3 H=SafeNormalizeCommon(L+V,N);
    float NoL=saturate(dot(N,L)), NoV=max(saturate(dot(N,V)),1e-4f);
    float NoH=saturate(dot(N,H)), VoH=saturate(dot(V,H));
    float roughness=clamp(surface.Roughness,0.02f,1.0f);
    float a=roughness*roughness, a2=a*a;
    float d=NoH*NoH*(a2-1.0f)+1.0f;
    float D=a2/max(PI*d*d,1e-6f);
    float k=(roughness+1.0f)*(roughness+1.0f)*0.125f;
    float G=(NoL/max(NoL*(1-k)+k,1e-5f))*(NoV/max(NoV*(1-k)+k,1e-5f));
    float ratio=(surface.IOR-1.0f)/(surface.IOR+1.0f);
    float dielectricF0=ratio*ratio;
    float3 f0=lerp(float3(dielectricF0,dielectricF0,dielectricF0),surface.BaseColor,saturate(surface.Metallic));
    float3 F=f0+(1-f0)*pow(1-VoH,5);
    float3 specular=D*G*F/max(4*NoL*NoV,1e-5f);
    float3 diffuse=(1-F)*surface.BaseColor*(1-saturate(surface.Transmission))*(1-saturate(surface.Metallic))/PI;
    return (specular+diffuse)*NoL*float3(1,0.96f,0.9f)*max(SceneRayParameters.y,0.0f);
}

float3 EvaluateMaterialOpticsPreview(SurfaceData surface, float3 position, float3 viewDir)
{
    uint shape=(uint)round(SceneRayParameters.z);
    float3 incident=-viewDir;
    float3 N=dot(surface.NormalWS,viewDir)>=0 ? surface.NormalWS : -surface.NormalWS;
    float ior=max(surface.IOR,1.0f);
    float F=GlassFresnel(dot(N,viewDir),1,ior);
    float metallic=saturate(surface.Metallic), transmission=saturate(surface.Transmission);
    const bool glassMaterial=transmission>0.0f;
    float3 reflection=0.0f;
    float3 result;
    if (glassMaterial)
        result=MaterialStudioDirect(surface,N,viewDir);
    else
    {
        reflection=MaterialStudioEnvironment(reflect(incident,N),surface.Roughness);
        result=F*reflection+(1-F)*surface.BaseColor*MaterialStudioEnvironment(N,1);
    }
    float3 direction=refract(incident,N,1/ior);
    if (transmission > 0.0f && dot(direction,direction)>1e-8f)
    {
        direction=normalize(direction);
        float3 origin=position+direction*0.0001f;
        float3 throughput=(1-F)*transmission*(1-metallic);
        [loop] for(uint bounce=0;bounce<8;++bounce)
        {
            float distance; float3 outward;
            if (!MaterialPreviewExit(origin,direction,shape,surface.Thickness,distance,outward)) break;
            throughput*=GlassBeer(surface.BaseColor,surface.AbsorptionColor,surface.Thickness,distance);
            float3 exitPosition=origin+direction*distance;
            float3 exitNormal=-outward;
            float exitF=GlassFresnel(-dot(direction,exitNormal),ior,1);
            float3 transmitted=refract(direction,exitNormal,ior);
            if (dot(transmitted,transmitted)>1e-8f)
                result+=throughput*(1-exitF)*MaterialStudioTransmission(exitPosition,normalize(transmitted),saturate(Transparent0.z));
            throughput*=exitF;
            if (max(throughput.x,max(throughput.y,throughput.z))<1e-4f || shape==3) break;
            direction=normalize(reflect(direction,exitNormal));
            origin=exitPosition+direction*0.0001f;
        }
    }
    if (glassMaterial) return result+surface.Emissive;
    return (1-metallic)*result+metallic*surface.BaseColor*reflection+surface.Emissive;
}
#endif
