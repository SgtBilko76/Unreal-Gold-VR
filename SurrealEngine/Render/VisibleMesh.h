#pragma once

#include "Math/mat.h"

class UActor;
class UMesh;
class USkeletalMesh;
struct MeshFace;
class ULodMesh;
class VisibleFrame;

struct BlendInfo
{
	int offsets[3];
	float t0, t1;
	float weight;
};

class VisibleMesh
{
public:
	bool DrawMesh(VisibleFrame* frame, UActor* actor, bool wireframe, bool translucentPass);

	// Optional (VR muzzle flash): track the drawn vertex that reaches furthest along TipDir from
	// TipOrigin, in world space - i.e. the barrel tip of a held weapon in its current pose.
	bool TrackTip = false;
	vec3 TipOrigin = vec3(0.0f);
	vec3 TipDir = vec3(1.0f, 0.0f, 0.0f);
	float TipBest = -1e30f;
	vec3 Tip = vec3(0.0f);
	void TrackTipPoint(const vec3& p)
	{
		if (!TrackTip)
			return;
		float d = dot(p - TipOrigin, TipDir);
		if (d > TipBest)
		{
			TipBest = d;
			Tip = p;
		}
	}

private:
	void DrawDebugInfo(VisibleFrame* frame, UActor* actor);
	bool DrawMeshAtLocation(VisibleFrame* frame, UActor* actor, UActor* lightLocationActor, UMesh* mesh, const mat4& meshToWorld, const mat3& meshNormalToWorld, bool translucentPass);
	bool DrawMesh(VisibleFrame* frame, UActor* actor, UActor* lightLocationActor, UMesh* mesh, const mat4& ObjectToWorld, const mat3& ObjectNormalToWorld, bool translucentPass);
	bool DrawMeshDX(VisibleFrame* frame, UActor* actor, UActor* lightLocationActor, UMesh* mesh, const mat4& ObjectToWorld, const mat3& ObjectNormalToWorld, bool translucentPass);
	bool DrawLodMesh(VisibleFrame* frame, UActor* actor, UActor* lightLocationActor, ULodMesh* mesh, const mat4& ObjectToWorld, const mat3& ObjectNormalToWorld, bool translucentPass);
	bool DrawLodMeshDX(VisibleFrame* frame, UActor* actor, UActor* lightLocationActor, ULodMesh* mesh, const mat4& ObjectToWorld, const mat3& ObjectNormalToWorld, bool translucentPass);
	bool DrawLodMeshFace(VisibleFrame* frame, UActor* actor, UActor* lightLocationActor, ULodMesh* mesh, const Array<MeshFace>& faces, const mat4& ObjectToWorld, const mat3& ObjectNormalToWorld, int baseVertexOffset, const int* vertexOffsets, float t0, float t1, bool translucentPass);
	bool DrawLodMeshFaceDX(VisibleFrame* frame, UActor* actor, UActor* lightLocationActor, ULodMesh* mesh, const Array<MeshFace>& faces, const mat4& ObjectToWorld, const mat3& ObjectNormalToWorld, int baseVertexOffset, const int* vertexOffsets, float t0, float t1, bool translucentPass, BlendInfo* blends, int blendCount);
	bool DrawSkeletalMesh(VisibleFrame* frame, UActor* actor, UActor* lightLocationActor, USkeletalMesh* mesh, const mat4& ObjectToWorld, const mat3& ObjectNormalToWorld, bool translucentPass);
	void SetupMeshTextures(UActor* actor, UMesh* mesh);
	void SetupLodMeshTextures(UActor* actor, ULodMesh* mesh);
	void FindAttachmentPoints(ULodMesh* mesh, const mat4& ObjectToWorld, const int* vertexOffsets, float t0, float t1);

	Array<vec3> attachmentTris;
};
