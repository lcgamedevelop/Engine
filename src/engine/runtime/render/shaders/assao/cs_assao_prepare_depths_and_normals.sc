/*
 * Adapted from bgfx examples/39-assao at
 * 7a70927d2af7ffcc5cef9c9e8f4c821e31fcb099. Fixed quality 2, two smart blur passes.
 * Changes: private uniforms, orthographic reconstruction, safe border reads,
 * and linear visibility for forward indirect lighting. The sampling pattern,
 * depth mip weighting, edge detection and smart filtering remain upstream.
 * License and change details: third_party/notices/bgfx-assao.md.
 */

/*
 * Copyright 2018 Attila Kocsis. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

#include "bgfx_compute.sh"
#include "uniforms.sh"

SAMPLER2D(s_depthSource, 0);

IMAGE2D_WO(s_target0, r16f, 1);
IMAGE2D_WO(s_target1, r16f, 2);
IMAGE2D_WO(s_target2, r16f, 3);
IMAGE2D_WO(s_target3, r16f, 4);
IMAGE2D_WO(s_normalsOutputUAV, rgba8, 5);

float ScreenSpaceToViewSpaceDepth( float screenDepth )
{
    float depthLinearizeMul = u_depthUnpackConsts.x;
    float depthLinearizeAdd = u_depthUnpackConsts.y;

    // Optimised version of "-cameraClipNear / (cameraClipFar - projDepth * (cameraClipFar - cameraClipNear)) * cameraClipFar"

    // Set your depthLinearizeMul and depthLinearizeAdd to:
    // depthLinearizeMul = ( cameraClipFar * cameraClipNear) / ( cameraClipFar - cameraClipNear );
    // depthLinearizeAdd = cameraClipFar / ( cameraClipFar - cameraClipNear );

    float viewspaceDepth = u_orthographic > 0.5
        ? depthLinearizeAdd + screenDepth * depthLinearizeMul
        : depthLinearizeMul / max( depthLinearizeAdd - screenDepth, 1.0e-20 );
    // R16F intermediates must remain finite even for a very distant far plane.
    return clamp(viewspaceDepth, 1.0e-5, 65504.0);
}

vec3 NDCToViewspace( vec2 pos, float viewspaceDepth )
{
    vec3 ret;

    float projectionScale = u_orthographic > 0.5 ? 1.0 : viewspaceDepth;
    ret.xy = (u_ndcToViewMul * pos.xy + u_ndcToViewAdd) * projectionScale;

    ret.z = viewspaceDepth;

    return ret;
}

vec4 CalculateEdges( const float centerZ, const float leftZ, const float rightZ, const float topZ, const float bottomZ )
{
    // slope-sensitive depth-based edge detection
    vec4 edgesLRTB = vec4( leftZ, rightZ, topZ, bottomZ ) - centerZ;
    vec4 edgesLRTBSlopeAdjusted = edgesLRTB + edgesLRTB.yxwz;
    edgesLRTB = min( abs( edgesLRTB ), abs( edgesLRTBSlopeAdjusted ) );
    return saturate( ( 1.3 - edgesLRTB / (centerZ * 0.040) ) );

    // cheaper version but has artifacts
    // edgesLRTB = abs( vec4( leftZ, rightZ, topZ, bottomZ ) - centerZ; );
    // return saturate( ( 1.3 - edgesLRTB / (pixZ * 0.06 + 0.1) ) );
}

vec3 CalculateNormal( const vec4 edgesLRTB, vec3 pixCenterPos, vec3 pixLPos, vec3 pixRPos, vec3 pixTPos, vec3 pixBPos )
{
    // Get this pixel's viewspace normal
    vec4 acceptedNormals  = vec4( edgesLRTB.x*edgesLRTB.z, edgesLRTB.z*edgesLRTB.y, edgesLRTB.y*edgesLRTB.w, edgesLRTB.w*edgesLRTB.x );

    pixLPos = normalize(pixLPos - pixCenterPos);
    pixRPos = normalize(pixRPos - pixCenterPos);
    pixTPos = normalize(pixTPos - pixCenterPos);
    pixBPos = normalize(pixBPos - pixCenterPos);

    vec3 pixelNormal = vec3( 0, 0, -0.0005 );
    pixelNormal += ( acceptedNormals.x ) * cross( pixLPos, pixTPos );
    pixelNormal += ( acceptedNormals.y ) * cross( pixTPos, pixRPos );
    pixelNormal += ( acceptedNormals.z ) * cross( pixRPos, pixBPos );
    pixelNormal += ( acceptedNormals.w ) * cross( pixBPos, pixLPos );
    pixelNormal = normalize( pixelNormal );

    return pixelNormal;
}

NUM_THREADS(8, 8, 1)
void main()
{
	uvec2 dtID = uvec2(gl_GlobalInvocationID.xy);

	uvec2 dim = imageSize(s_target0).xy;
	if (all(lessThan(dtID.xy, dim) ) )
	{
		ivec2 baseCoords = ivec2(dtID.xy) * 2;
#if BGFX_SHADER_LANGUAGE_GLSL
		vec2 upperLeftUV = (vec2(dtID.xy) + vec2(0.25,0.75)) * u_viewport2xPixelSize;
#else
		vec2 upperLeftUV = (vec2(dtID.xy) + vec2(0.25,0.25)) * u_viewport2xPixelSize;
#endif

        // Gather at integer texel boundaries. Each fetch supplies one side of the
        // same 2x2 output quad and its one-texel halo, without filtering the depth.
        vec2 texel_size = vec2_splat(1.0) / vec2(textureSize(s_depthSource, 0));
        vec2 base = vec2(baseCoords);
        vec4 left = textureGather(s_depthSource, (base + vec2(0.0, 1.0)) * texel_size, 0);
        vec4 right = textureGather(s_depthSource, (base + vec2(2.0, 1.0)) * texel_size, 0);
        vec2 earlier_row = textureGather(s_depthSource, (base + vec2(1.0, 0.0)) * texel_size, 0).wz;
        vec2 later_row = textureGather(s_depthSource, (base + vec2(1.0, 2.0)) * texel_size, 0).xy;
#if BGFX_SHADER_LANGUAGE_GLSL
        left = left.wzyx;
        right = right.wzyx;
        vec2 top = later_row;
        vec2 bottom = earlier_row;
#else
        vec2 top = earlier_row;
        vec2 bottom = later_row;
#endif
        float pixZs[4][4];
        pixZs[1][1] = ScreenSpaceToViewSpaceDepth(left.z);
        pixZs[2][1] = ScreenSpaceToViewSpaceDepth(right.w);
        pixZs[1][2] = ScreenSpaceToViewSpaceDepth(left.y);
        pixZs[2][2] = ScreenSpaceToViewSpaceDepth(right.x);
        pixZs[0][1] = ScreenSpaceToViewSpaceDepth(left.w);
        pixZs[0][2] = ScreenSpaceToViewSpaceDepth(left.x);
        pixZs[3][1] = ScreenSpaceToViewSpaceDepth(right.z);
        pixZs[3][2] = ScreenSpaceToViewSpaceDepth(right.y);
        pixZs[1][0] = ScreenSpaceToViewSpaceDepth(top.x);
        pixZs[2][0] = ScreenSpaceToViewSpaceDepth(top.y);
        pixZs[1][3] = ScreenSpaceToViewSpaceDepth(bottom.x);
        pixZs[2][3] = ScreenSpaceToViewSpaceDepth(bottom.y);

        imageStore(s_target0, ivec2(dtID), vec4_splat(pixZs[1][1]));
        imageStore(s_target1, ivec2(dtID), vec4_splat(pixZs[2][1]));
        imageStore(s_target2, ivec2(dtID), vec4_splat(pixZs[1][2]));
        imageStore(s_target3, ivec2(dtID), vec4_splat(pixZs[2][2]));

		vec4 edges0 = CalculateEdges( pixZs[1][1], pixZs[0][1], pixZs[2][1], pixZs[1][0], pixZs[1][2] );
		vec4 edges1 = CalculateEdges( pixZs[2][1], pixZs[1][1], pixZs[3][1], pixZs[2][0], pixZs[2][2] );
		vec4 edges2 = CalculateEdges( pixZs[1][2], pixZs[0][2], pixZs[2][2], pixZs[1][1], pixZs[1][3] );
		vec4 edges3 = CalculateEdges( pixZs[2][2], pixZs[1][2], pixZs[3][2], pixZs[2][1], pixZs[2][3] );

		vec2 viewportPixelSize = u_viewportPixelSize;
#if BGFX_SHADER_LANGUAGE_GLSL
		viewportPixelSize.y = -viewportPixelSize.y;
#endif

			vec3 pixPos[4][4];
		// middle 4
		pixPos[1][1] = NDCToViewspace( upperLeftUV + viewportPixelSize * vec2( 0.0,  0.0 ), pixZs[1][1] );
		pixPos[2][1] = NDCToViewspace( upperLeftUV + viewportPixelSize * vec2( 1.0,  0.0 ), pixZs[2][1] );
		pixPos[1][2] = NDCToViewspace( upperLeftUV + viewportPixelSize * vec2( 0.0,  1.0 ), pixZs[1][2] );
		pixPos[2][2] = NDCToViewspace( upperLeftUV + viewportPixelSize * vec2( 1.0,  1.0 ), pixZs[2][2] );
		// left 2
		pixPos[0][1] = NDCToViewspace( upperLeftUV + viewportPixelSize * vec2( -1.0,  0.0), pixZs[0][1] );
		pixPos[0][2] = NDCToViewspace( upperLeftUV + viewportPixelSize * vec2( -1.0,  1.0), pixZs[0][2] );
		// right 2
		pixPos[3][1] = NDCToViewspace( upperLeftUV + viewportPixelSize * vec2(  2.0,  0.0), pixZs[3][1] );
		pixPos[3][2] = NDCToViewspace( upperLeftUV + viewportPixelSize * vec2(  2.0,  1.0), pixZs[3][2] );
		// top 2
		pixPos[1][0] = NDCToViewspace( upperLeftUV + viewportPixelSize * vec2( 0.0, -1.0 ), pixZs[1][0] );
		pixPos[2][0] = NDCToViewspace( upperLeftUV + viewportPixelSize * vec2( 1.0, -1.0 ), pixZs[2][0] );
		// bottom 2
		pixPos[1][3] = NDCToViewspace( upperLeftUV + viewportPixelSize * vec2( 0.0,  2.0 ), pixZs[1][3] );
		pixPos[2][3] = NDCToViewspace( upperLeftUV + viewportPixelSize * vec2( 1.0,  2.0 ), pixZs[2][3] );

		vec3 norm0 = CalculateNormal( edges0, pixPos[1][1], pixPos[0][1], pixPos[2][1], pixPos[1][0], pixPos[1][2] );
		vec3 norm1 = CalculateNormal( edges1, pixPos[2][1], pixPos[1][1], pixPos[3][1], pixPos[2][0], pixPos[2][2] );
		vec3 norm2 = CalculateNormal( edges2, pixPos[1][2], pixPos[0][2], pixPos[2][2], pixPos[1][1], pixPos[1][3] );
		vec3 norm3 = CalculateNormal( edges3, pixPos[2][2], pixPos[1][2], pixPos[3][2], pixPos[2][1], pixPos[2][3] );

#if BGFX_SHADER_LANGUAGE_GLSL
		imageStore(s_normalsOutputUAV, baseCoords + ivec2( 0, 1 ), vec4( norm0 * 0.5 + 0.5, 0.0 ));
		imageStore(s_normalsOutputUAV, baseCoords + ivec2( 1, 1 ), vec4( norm1 * 0.5 + 0.5, 0.0 ));
		imageStore(s_normalsOutputUAV, baseCoords + ivec2( 0, 0 ), vec4( norm2 * 0.5 + 0.5, 0.0 ));
		imageStore(s_normalsOutputUAV, baseCoords + ivec2( 1, 0 ), vec4( norm3 * 0.5 + 0.5, 0.0 ));
#else
		imageStore(s_normalsOutputUAV, baseCoords + ivec2( 0, 0 ), vec4( norm0 * 0.5 + 0.5, 0.0 ));
		imageStore(s_normalsOutputUAV, baseCoords + ivec2( 1, 0 ), vec4( norm1 * 0.5 + 0.5, 0.0 ));
		imageStore(s_normalsOutputUAV, baseCoords + ivec2( 0, 1 ), vec4( norm2 * 0.5 + 0.5, 0.0 ));
		imageStore(s_normalsOutputUAV, baseCoords + ivec2( 1, 1 ), vec4( norm3 * 0.5 + 0.5, 0.0 ));
#endif
	}
}
