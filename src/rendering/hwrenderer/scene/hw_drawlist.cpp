// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2002-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** hw_drawlist.cpp
** The main container type for draw items.
**
*/

#include "r_sky.h"
#include "r_utility.h"
#include "doomstat.h"
#include "actor.h"
#include "g_levellocals.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_drawlist.h"
#include "flatvertices.h"
#include "hw_clock.h"
#include "hw_renderstate.h"
#include "hw_drawinfo.h"
#include "hw_fakeflat.h"
#include "hw_walldispatcher.h"

#include "g_cvars.h" // for "gl_lights" cvar
#include "common/rendering/gl_load/gl_load.h"
#include "common/rendering/gl/gl_renderstate.h"
#include "common/rendering/gl_load/gl_interface.h" // to get "if (gl.legacyMode) context
#include "common/rendering/hwrenderer/data/hw_cvars.h"
#include "common/rendering/gl/gl_20.h"

FMemArena RenderDataAllocator(1024*1024);	// Use large blocks to reduce allocation time.

//extern OpenGLRenderer::HWWall* g_isCurrentlyHWWallDrawing; // Tracks current processing wall
//extern OpenGLRenderer::HWFlat* g_isCurrentlyHWFlatDrawing; // Tracks current processing flat
extern HWWall* g_isCurrentlyHWWallDrawing; // Tracks current processing wall
extern HWFlat* g_isCurrentlyHWFlatDrawing; // Tracks current processing flat

void ResetRenderDataAllocator()
{
	RenderDataAllocator.FreeAll();
}

//==========================================================================
//
//
//
//==========================================================================
class StaticSortNodeArray : public TDeletingArray<SortNode*>
{
	unsigned usecount;
public:
	unsigned Size() { return usecount; }
	void Clear() { usecount=0; }
	void Release(int start) { usecount=start; }
	SortNode * GetNew();
};


SortNode * StaticSortNodeArray::GetNew()
{
	if (usecount==TArray<SortNode*>::Size())
	{
		Push(new SortNode);
	}
	return operator[](usecount++);
}


static StaticSortNodeArray SortNodes;

//==========================================================================
//                            DoDraw cache
// Cache cumulative translucency color mask decisions for flat surfaces.
// Refreshes the structural validation state once every 8 map ticks.
//
//==========================================================================
const bool useDoDrawTranslucCumulDetectCache = false; // Just for fun and hardcore diagnostic testing
struct FTranslucencyGL1xCacheEntry
{
	int  lastUpdateTick = -1;
	bool maskColorChannelsOut = false;
};
static TMap<int, FTranslucencyGL1xCacheEntry> g_TranslucencyGL1xOverdrawCache;

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::Reset()
{
	if (sorted) SortNodes.Release(SortNodeStart);
	sorted=NULL;
	walls.Clear();
	flats.Clear();
	sprites.Clear();
	drawitems.Clear();
	if (useDoDrawTranslucCumulDetectCache)
	{
		g_TranslucencyGL1xOverdrawCache.Clear();
	}
}

//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::UnlinkFromChain()
{
	if (parent) parent->next=next;
	if (next) next->parent=parent;
	parent=next=NULL;
}

//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::Link(SortNode * hook)
{
	if (hook)
	{
		parent=hook->parent;
		hook->parent=this;
	}
	next=hook;
	if (parent) parent->next=this;
}

//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::AddToEqual(SortNode *child)
{
	child->UnlinkFromChain();
	child->equal=equal;
	equal=child;
}

//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::AddToLeft(SortNode * child)
{
	child->UnlinkFromChain();
	child->Link(left);
	left=child;
}

//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::AddToRight(SortNode * child)
{
	child->UnlinkFromChain();
	child->Link(right);
	right=child;
}


//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::MakeSortList()
{
	SortNode * p, * n, * c;
	unsigned i;

	SortNodeStart=SortNodes.Size();
	p=NULL;
	n=SortNodes.GetNew();
	for(i=0;i<drawitems.Size();i++)
	{
		n->itemindex=(int)i;
		n->left=n->equal=n->right=NULL;
		n->parent=p;
		p=n;
		if (i!=drawitems.Size()-1)
		{
			c=SortNodes.GetNew();
			n->next=c;
			n=c;
		}
		else
		{
			n->next=NULL;
		}
	}
}


//==========================================================================
//
//
//
//==========================================================================
SortNode * HWDrawList::FindSortPlane(SortNode * head)
{
	while (head->next && drawitems[head->itemindex].rendertype!=DrawType_FLAT) 
		head=head->next;
	if (drawitems[head->itemindex].rendertype==DrawType_FLAT) return head;
	return NULL;
}


//==========================================================================
//
//
//
//==========================================================================
SortNode * HWDrawList::FindSortWall(SortNode * head)
{
	float farthest = -FLT_MAX;
	float nearest = FLT_MAX;
	SortNode * best = NULL;
	SortNode * node = head;
	float bestdist = FLT_MAX;

	while (node)
	{
		HWDrawItem * it = &drawitems[node->itemindex];
		if (it->rendertype == DrawType_WALL)
		{
			float d = walls[it->index]->ViewDistance;
			if (d > farthest) farthest = d;
			if (d < nearest) nearest = d;
		}
		node = node->next;
	}
	if (farthest == INT_MIN) return NULL;
	node = head;
	farthest = (farthest + nearest) / 2;
	while (node)
	{
		HWDrawItem * it = &drawitems[node->itemindex];
		if (it->rendertype == DrawType_WALL)
		{
			float di = fabsf(walls[it->index]->ViewDistance - farthest);
			if (!best || di < bestdist)
			{
				best = node;
				bestdist = di;
			}
		}
		node = node->next;
	}
	return best;
}

//==========================================================================
//
// Note: sloped planes are a huge problem...
//
//==========================================================================
void HWDrawList::SortPlaneIntoPlane(SortNode * head,SortNode * sort)
{
	HWFlat * fh= flats[drawitems[head->itemindex].index];
	HWFlat * fs= flats[drawitems[sort->itemindex].index];

	if (fh->z==fs->z) 
		head->AddToEqual(sort);
	else if ( (fh->z<fs->z && fh->ceiling) || (fh->z>fs->z && !fh->ceiling)) 
		head->AddToLeft(sort);
	else 
		head->AddToRight(sort);
}


//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::SortWallIntoPlane(HWDrawInfo* di, SortNode * head, SortNode * sort)
{
	HWFlat * fh = flats[drawitems[head->itemindex].index];
	HWWall * ws = walls[drawitems[sort->itemindex].index];

	bool ceiling = fh->z > SortZ;

	if ((ws->ztop[0] > fh->z || ws->ztop[1] > fh->z) && (ws->zbottom[0] < fh->z || ws->zbottom[1] < fh->z))
	{
		// We have to split this wall!

		HWWall *w = NewWall();
		*w = *ws;

		// Splitting is done in the shader with clip planes, if available
		if (screen->hwcaps & RFL_NO_CLIP_PLANES)
		{
			ws->vertcount = 0;	// invalidate current vertices.
			float newtexv = ws->tcs[HWWall::UPLFT].v + ((ws->tcs[HWWall::LOLFT].v - ws->tcs[HWWall::UPLFT].v) / (ws->zbottom[0] - ws->ztop[0])) * (fh->z - ws->ztop[0]);
			float newlmv = ws->lightuv[HWWall::UPLFT].v + ((ws->lightuv[HWWall::LOLFT].v - ws->lightuv[HWWall::UPLFT].v) / (ws->zbottom[0] - ws->ztop[0])) * (fh->z - ws->ztop[0]);

			// I make the very big assumption here that translucent walls in sloped sectors
			// and 3D-floors never coexist in the same level - If that were the case this
			// code would become extremely more complicated.
			if (!ceiling)
			{
				ws->ztop[1] = w->zbottom[1] = ws->ztop[0] = w->zbottom[0] = fh->z;
				ws->tcs[HWWall::UPRGT].v = w->tcs[HWWall::LORGT].v = ws->tcs[HWWall::UPLFT].v = w->tcs[HWWall::LOLFT].v = newtexv;
				ws->lightuv[HWWall::UPRGT].v = w->lightuv[HWWall::LORGT].v = ws->lightuv[HWWall::UPLFT].v = w->lightuv[HWWall::LOLFT].v = newlmv;
			}
			else
			{
				w->ztop[1] = ws->zbottom[1] = w->ztop[0] = ws->zbottom[0] = fh->z;
				w->tcs[HWWall::UPLFT].v = ws->tcs[HWWall::LOLFT].v = w->tcs[HWWall::UPRGT].v = ws->tcs[HWWall::LORGT].v = newtexv;
				w->lightuv[HWWall::UPLFT].v = ws->lightuv[HWWall::LOLFT].v = w->lightuv[HWWall::UPRGT].v = ws->lightuv[HWWall::LORGT].v = newlmv;
			}
			w->MakeVertices(false);
			ws->MakeVertices(false);
		}

		SortNode * sort2 = SortNodes.GetNew();
		memset(sort2, 0, sizeof(SortNode));
		sort2->itemindex = drawitems.Size() - 1;

		head->AddToLeft(sort);
		head->AddToRight(sort2);
	}
	else if ((ws->zbottom[0] < fh->z && !ceiling) || (ws->ztop[0] > fh->z && ceiling))	// completely on the left side
	{
		head->AddToLeft(sort);
	}
	else
	{
		head->AddToRight(sort);
	}

}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::SortSpriteIntoPlane(SortNode * head, SortNode * sort)
{
	HWFlat * fh = flats[drawitems[head->itemindex].index];
	HWSprite * ss = sprites[drawitems[sort->itemindex].index];

	bool ceiling = fh->z > SortZ;

	auto hiz = ss->z1 > ss->z2 ? ss->z1 : ss->z2;
	auto loz = ss->z1 < ss->z2 ? ss->z1 : ss->z2;

	if ((hiz > fh->z && loz < fh->z) || ss->modelframe)
	{
		// We have to split this sprite
		HWSprite *s = NewSprite();
		*s = *ss;

		// Splitting is done in the shader with clip planes, if available.
		// The fallback here only really works for non-y-billboarded sprites.
		if (screen->hwcaps & RFL_NO_CLIP_PLANES)
		{
			float newtexv = ss->vt + ((ss->vb - ss->vt) / (ss->z2 - ss->z1))*(fh->z - ss->z1);

			if (!ceiling)
			{
				ss->z1 = s->z2 = fh->z;
				ss->vt = s->vb = newtexv;
			}
			else
			{
				s->z1 = ss->z2 = fh->z;
				s->vt = ss->vb = newtexv;
			}
		}

		SortNode * sort2 = SortNodes.GetNew();
		memset(sort2, 0, sizeof(SortNode));
		sort2->itemindex = drawitems.Size() - 1;

		head->AddToLeft(sort);
		head->AddToRight(sort2);
	}
	else if ((ss->z2<fh->z && !ceiling) || (ss->z1>fh->z && ceiling))	// completely on the left side
	{
		head->AddToLeft(sort);
	}
	else
	{
		head->AddToRight(sort);
	}
}

//==========================================================================
//
//
//
//==========================================================================
#define MIN_EQ (0.0005f)

// Lines start-end and fdiv must intersect.
inline double CalcIntersectionVertex(HWWall *w1, HWWall * w2)
{
	float ax = w1->glseg.x1, ay = w1->glseg.y1;
	float bx = w1->glseg.x2, by = w1->glseg.y2;
	float cx = w2->glseg.x1, cy = w2->glseg.y1;
	float dx = w2->glseg.x2, dy = w2->glseg.y2;
	return ((ay - cy)*(dx - cx) - (ax - cx)*(dy - cy)) / ((bx - ax)*(dy - cy) - (by - ay)*(dx - cx));
}

void HWDrawList::SortWallIntoWall(HWDrawInfo *di, SortNode * head,SortNode * sort)
{
	HWWall * wh= walls[drawitems[head->itemindex].index];
	HWWall * ws= walls[drawitems[sort->itemindex].index];
	float v1=wh->PointOnSide(ws->glseg.x1,ws->glseg.y1);
	float v2=wh->PointOnSide(ws->glseg.x2,ws->glseg.y2);

	if (fabs(v1)<MIN_EQ && fabs(v2)<MIN_EQ) 
	{
		if (ws->type==RENDERWALL_FOGBOUNDARY && wh->type!=RENDERWALL_FOGBOUNDARY) 
		{
			head->AddToRight(sort);
		}
		else if (ws->type!=RENDERWALL_FOGBOUNDARY && wh->type==RENDERWALL_FOGBOUNDARY) 
		{
			head->AddToLeft(sort);
		}
		else 
		{
			head->AddToEqual(sort);
		}
	}
	else if (v1<MIN_EQ && v2<MIN_EQ) 
	{
		head->AddToLeft(sort);
	}
	else if (v1>-MIN_EQ && v2>-MIN_EQ) 
	{
		head->AddToRight(sort);
	}
	else
	{
		double r = CalcIntersectionVertex(ws, wh);

		float ix=(float)(ws->glseg.x1+r*(ws->glseg.x2-ws->glseg.x1));
		float iy=(float)(ws->glseg.y1+r*(ws->glseg.y2-ws->glseg.y1));
		float iu=(float)(ws->tcs[HWWall::UPLFT].u + r * (ws->tcs[HWWall::UPRGT].u - ws->tcs[HWWall::UPLFT].u));
		float ilmu=(float)(ws->lightuv[HWWall::UPLFT].u + r * (ws->lightuv[HWWall::UPRGT].u - ws->lightuv[HWWall::UPLFT].u));
		float izt=(float)(ws->ztop[0]+r*(ws->ztop[1]-ws->ztop[0]));
		float izb=(float)(ws->zbottom[0]+r*(ws->zbottom[1]-ws->zbottom[0]));

		ws->vertcount = 0;	// invalidate current vertices.
		HWWall *w= NewWall();
		*w = *ws;

		w->glseg.x1=ws->glseg.x2=ix;
		w->glseg.y1=ws->glseg.y2=iy;
		w->glseg.fracleft = ws->glseg.fracright = ws->glseg.fracleft + r*(ws->glseg.fracright - ws->glseg.fracleft);
		w->ztop[0]=ws->ztop[1]=izt;
		w->zbottom[0]=ws->zbottom[1]=izb;
		w->tcs[HWWall::LOLFT].u = w->tcs[HWWall::UPLFT].u = ws->tcs[HWWall::LORGT].u = ws->tcs[HWWall::UPRGT].u = iu;
		w->lightuv[HWWall::LOLFT].u = w->lightuv[HWWall::UPLFT].u = ws->lightuv[HWWall::LORGT].u = ws->lightuv[HWWall::UPRGT].u = iu;
		ws->MakeVertices(false);
		w->MakeVertices(false);

		SortNode * sort2=SortNodes.GetNew();
		memset(sort2,0,sizeof(SortNode));
		sort2->itemindex=drawitems.Size()-1;

		if (v1>0)
		{
			head->AddToLeft(sort2);
			head->AddToRight(sort);
		}
		else
		{
			head->AddToLeft(sort);
			head->AddToRight(sort2);
		}
	}
}


//==========================================================================
//
// 
//
//==========================================================================
EXTERN_CVAR(Int, gl_billboard_mode)
EXTERN_CVAR(Bool, gl_billboard_faces_camera)
EXTERN_CVAR(Bool, hw_force_cambbpref)
EXTERN_CVAR(Bool, gl_billboard_particles)

inline double CalcIntersectionVertex(HWSprite *s, HWWall * w2)
{
	float ax = s->x1, ay = s->y1;
	float bx = s->x2, by = s->y2;
	float cx = w2->glseg.x1, cy = w2->glseg.y1;
	float dx = w2->glseg.x2, dy = w2->glseg.y2;
	return ((ay - cy)*(dx - cx) - (ax - cx)*(dy - cy)) / ((bx - ax)*(dy - cy) - (by - ay)*(dx - cx));
}

void HWDrawList::SortSpriteIntoWall(HWDrawInfo *di, SortNode * head,SortNode * sort)
{
	HWWall *wh= walls[drawitems[head->itemindex].index];
	HWSprite * ss= sprites[drawitems[sort->itemindex].index];

	float v1 = wh->PointOnSide(ss->x1, ss->y1);
	float v2 = wh->PointOnSide(ss->x2, ss->y2);

	if (fabs(v1)<MIN_EQ && fabs(v2)<MIN_EQ) 
	{
		if (wh->type==RENDERWALL_FOGBOUNDARY) 
		{
			head->AddToLeft(sort);
		}
		else 
		{
			head->AddToEqual(sort);
		}
	}
	else if (v1<MIN_EQ && v2<MIN_EQ) 
	{
		head->AddToLeft(sort);
	}
	else if (v1>-MIN_EQ && v2>-MIN_EQ) 
	{
		head->AddToRight(sort);
	}
	else
	{
		const bool drawWithXYBillboard = ((ss->particle && gl_billboard_particles) || (!(ss->actor && ss->actor->renderflags & RF_FORCEYBILLBOARD)
			&& (gl_billboard_mode == 1 || (ss->actor && ss->actor->renderflags & RF_FORCEXYBILLBOARD))));

		const bool drawBillboardFacingCamera = hw_force_cambbpref ? gl_billboard_faces_camera :
			(gl_billboard_faces_camera && (ss->actor && !(ss->actor->renderflags2 & RF2_BILLBOARDNOFACECAMERA)))
			|| (ss->actor && ss->actor->renderflags2 & RF2_BILLBOARDFACECAMERA);

		// [Nash] has +ROLLSPRITE
		const bool rotated = (ss->actor != nullptr && ss->actor->renderflags & (RF_ROLLSPRITE | RF_WALLSPRITE | RF_FLATSPRITE));

		// cannot sort them at the moment. This requires more complex splitting.
		if (drawWithXYBillboard || drawBillboardFacingCamera || rotated)
		{
			float v1 = wh->PointOnSide(ss->x, ss->y);
			if (v1 < 0)
			{
				head->AddToLeft(sort);
			}
			else
			{
				head->AddToRight(sort);
			}
			return;
		}
		double r=CalcIntersectionVertex(ss, wh);

		float ix=(float)(ss->x1 + r * (ss->x2-ss->x1));
		float iy=(float)(ss->y1 + r * (ss->y2-ss->y1));
		float iu=(float)(ss->ul + r * (ss->ur-ss->ul));

		HWSprite *s = NewSprite();
		*s = *ss;

		s->x1=ss->x2=ix;
		s->y1=ss->y2=iy;
		s->ul=ss->ur=iu;

		SortNode * sort2=SortNodes.GetNew();
		memset(sort2,0,sizeof(SortNode));
		sort2->itemindex=drawitems.Size()-1;

		if (v1>0)
		{
			head->AddToLeft(sort2);
			head->AddToRight(sort);
		}
		else
		{
			head->AddToLeft(sort);
			head->AddToRight(sort2);
		}
		if (screen->BuffersArePersistent())
		{
			s->vertexindex = ss->vertexindex = -1;
		}
		else
		{
			s->CreateVertices(di);
			ss->CreateVertices(di);
		}

	}
}


//==========================================================================
//
//
//
//==========================================================================

inline int HWDrawList::CompareSprites(SortNode * a,SortNode * b)
{
	HWSprite * s1= sprites[drawitems[a->itemindex].index];
	HWSprite * s2= sprites[drawitems[b->itemindex].index];

	if (s1->depth < s2->depth) return 1;
	if (s1->depth > s2->depth) return -1;
	return reverseSort? s2->index-s1->index : s1->index-s2->index;
}

//==========================================================================
//
//
//
//==========================================================================
SortNode * HWDrawList::SortSpriteList(SortNode * head)
{
	SortNode * n;
	int count;
	unsigned i;

	static TArray<SortNode*> sortspritelist;

	SortNode * parent=head->parent;

	sortspritelist.Clear();
	for(count=0,n=head;n;n=n->next) sortspritelist.Push(n);
	std::stable_sort(sortspritelist.begin(), sortspritelist.end(), [=](SortNode *a, SortNode *b)
	{
		return CompareSprites(a, b) < 0;
	});

	for(i=0;i<sortspritelist.Size();i++)
	{
		sortspritelist[i]->next=NULL;
		if (parent) parent->equal=sortspritelist[i];
		parent=sortspritelist[i];
	}
	return sortspritelist[0];
}

//==========================================================================
//
//
//
//==========================================================================
SortNode * HWDrawList::DoSort(HWDrawInfo *di, SortNode * head)
{
	SortNode * node, * sn, * next;

	sn=FindSortPlane(head);
	if (sn)
	{
		if (sn==head) head=head->next;
		sn->UnlinkFromChain();
		node=head;
		head=sn;
		while (node)
		{
			next=node->next;
			switch(drawitems[node->itemindex].rendertype)
			{
			case DrawType_FLAT:
				SortPlaneIntoPlane(head,node);
				break;

			case DrawType_WALL:
				SortWallIntoPlane(di,head,node);
				break;

			case DrawType_SPRITE:
				SortSpriteIntoPlane(head,node);
				break;
			}
			node=next;
		}
	}
	else
	{
		sn=FindSortWall(head);
		if (sn)
		{
			if (sn==head) head=head->next;
			sn->UnlinkFromChain();
			node=head;
			head=sn;
			while (node)
			{
				next=node->next;
				switch(drawitems[node->itemindex].rendertype)
				{
				case DrawType_WALL:
					SortWallIntoWall(di, head,node);
					break;

				case DrawType_SPRITE:
					SortSpriteIntoWall(di, head, node);
					break;

				case DrawType_FLAT: break;
				}
				node=next;
			}
		}
		else 
		{
			return SortSpriteList(head);
		}
	}
	if (head->left) head->left=DoSort(di, head->left);
	if (head->right) head->right=DoSort(di, head->right);
	return sn;
}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::Sort(HWDrawInfo *di)
{
	reverseSort = !!(di->Level->i_compatflags & COMPATF_SPRITESORT);
    SortZ = di->Viewpoint.Pos.Z;
	MakeSortList();
	sorted = DoSort(di, SortNodes[SortNodeStart]);
}

//==========================================================================
//
// Sorting the drawitems first by texture and then by light level
//
//==========================================================================

void HWDrawList::SortWalls()
{
	if (drawitems.Size() > 1)
	{
		std::sort(drawitems.begin(), drawitems.end(), [=](const HWDrawItem &a, const HWDrawItem &b) -> bool
		{
			HWWall * w1 = walls[a.index];
			HWWall * w2 = walls[b.index];

			if (w1->texture != w2->texture) return w1->texture < w2->texture;
			return (w1->flags & 3) < (w2->flags & 3);

		});
	}
}

void HWDrawList::SortFlats()
{
	if (drawitems.Size() > 1)
	{
		std::sort(drawitems.begin(), drawitems.end(), [=](const HWDrawItem &a, const HWDrawItem &b)
		{
			HWFlat * w1 = flats[a.index];
			HWFlat* w2 = flats[b.index];
			return w1->texture < w2->texture;
		});
	}
}


//==========================================================================
//
//
//
//==========================================================================

HWWall *HWDrawList::NewWall()
{
	auto wall = (HWWall*)RenderDataAllocator.Alloc(sizeof(HWWall));
	drawitems.Push(HWDrawItem(DrawType_WALL, walls.Push(wall)));
	return wall;
}

//==========================================================================
//
//
//
//==========================================================================
HWFlat *HWDrawList::NewFlat()
{
	auto flat = (HWFlat*)RenderDataAllocator.Alloc(sizeof(HWFlat));
	drawitems.Push(HWDrawItem(DrawType_FLAT,flats.Push(flat)));
	return flat;
}

//==========================================================================
//
//
//
//==========================================================================
HWSprite *HWDrawList::NewSprite()
{	
	auto sprite = (HWSprite*)RenderDataAllocator.Alloc(sizeof(HWSprite));
	drawitems.Push(HWDrawItem(DrawType_SPRITE, sprites.Push(sprite)));
	return sprite;
}

//==========================================================================
//
// [Single-Pass Alpha/Z Sync & Feature Bypass]
//
// WHAT YOU MUST DO:
// 1. Maintain simultaneous Alpha Testing (GL_GREATER) and Depth Writes
//    (glDepthMask(GL_TRUE)) during the SINGLE native w->Draw() pass.
//    This bakes the precise pixel depth footprint of solid elements
//    (iron bars, frame nodes) directly into the hardware Z-buffer.
// 2. IMPORTANT: NEVER call w->Draw() twice inside STEP 1. Single-pass
//    execution prevents double alpha channel blending, completely
//    eradicating edge ghosting, artificial darkening, and camera shifts.
// 3. IMPORTANT: DO NOT reset walls[index].vertcount to 0 between passes!
//    By preserving vertcount, MakeVertices() inside subsequent draws
//    skips re-tessellation, forcing the GPU to reuse the exact same
//    vertex layout, crushing reverse-side float rounding drifts.
// 4. Keep glDepthFunc(GL_EQUAL) locked during STEP 2 lighting passes.
//    Since vertcount remains frozen, light overlay stages match the
//    baked VBO mesh byte-by-byte, bypassing heavy stencil states.
// 5. Explicitly invoke glPushAttrib(GL_STENCIL_BUFFER_BIT) on Step 1 entry
//    and glPopAttrib() on Step 2 exit ONLY when useDualPassAlphaGate
//    is validated by the presence of a true alpha-masked texture layer.
// 6. Non-masked transparent surfaces (colored glass, sector fields) 
//    where w->gltexture->tex->bMasked is false securely trigger the fast
//    bypass path, completely disabling heavy stencil stalls to return 
//    maximum pristine 60 FPS performance across the entire map traversal.
// 7. Force glStencilMask(0x80) and hard-wipe our stencil footprint inside
//    recovery cleanup gates. Flashing the 8th bitplane to zero before
//    bailing out eradicates rectangular cuts on BFG explosions entirely.
//
// WHAT YOU MUST NEVER DO:
// 1. NEVER execute global glClear(GL_STENCIL_BUFFER_BIT) without isolating
//    the 8th bit plane. Wiping portal recursion data blindfolds the engine.
// 2. NEVER invoke expensive glGetIntegerv or glIsEnabled queries outside
//    the validated useDualPassAlphaGate scope inside this hot loop.
//    Probing active hardware registers drops frame rates by roughly 20%.
// 3. NEVER clear walls[index].vertcount until the absolute end of the
//    lighting sub-passes inside STEP 2 recovery cleanup gates!
// 4. NEVER forget to restore glDepthFunc(GL_LEQUAL) and glDepthMask(false)
//    at the end of STEP 2 cleanup gates to prevent pipeline drifts.
//
//==========================================================================
void HWDrawList::DoDraw(HWDrawInfo *di, FRenderState &state, bool translucent, int i)
{
	int currentRenderType = drawitems[i].rendertype;
	int index = drawitems[i].index;
	static int  s_DynlightMenuToggleSavedGL1xVal = 1;
	static bool s_WereDynlightsToggledInGL1xMenu = false;
	const bool reconstructStencilOnlyOnTrig = true; // can be faster?

	// [Darkcrafter07] - Context mapping fallback pass resolution for legacy pipelines
	int currentGL1xPass = translucent ? GLPASS_TRANSLUCENT : GLPASS_PLAIN;

	if (reconstructStencilOnlyOnTrig)
	{
		if (gl_lights != s_DynlightMenuToggleSavedGL1xVal)
		{
			s_WereDynlightsToggledInGL1xMenu = true;
			s_DynlightMenuToggleSavedGL1xVal = gl_lights;
		}

		if (s_WereDynlightsToggledInGL1xMenu)
		{
			glEnable(GL_STENCIL_TEST);
			glStencilMask(0x80);
			glClearStencil(0);
			glClear(GL_STENCIL_BUFFER_BIT); // Wipe 8th bitplane clean once

			glPopAttrib(); // Safe recovery pop on state change

			glDisable(GL_STENCIL_TEST);
			glStencilMask(0xFF); // Restore native factory bitplanes for portals
			OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);

			s_WereDynlightsToggledInGL1xMenu = false; // Destroy trigger link immediately
		}
	}

	// --- [Intelligent Translucency Feature Detector] ---
	// Scan the wall primitive to confirm if it features any binary alpha prores holes.
	// Non-masked translucent surfaces (like colored glass or smooth transparency fields)
	// to BYPASS the heavy stencil array stack, giving about 3-5% speedup.
	bool hasAlphaHolesTexture = false;
	if (gl.legacyMode && translucent && currentRenderType == DrawType_WALL)
	{
		HWWall * w = walls[index];
		// Re-mapped to GZ4x texture pointer interface to prevent undefined gltexture
		if (w && w->texture != nullptr)
		{
			if (w->texture->isMasked())
			{
				hasAlphaHolesTexture = true;
			}
		}
	}

	// STEP 1: RENDER ORIGINAL SURFACE WITH DUAL-PASS DEPTH INJECTION
	const bool onlyLightOpaqueTextureAreas = true;
	bool useDualPassAlphaGate = (gl.legacyMode && translucent &&
		currentRenderType == DrawType_WALL && onlyLightOpaqueTextureAreas && hasAlphaHolesTexture);

	// Determine active hardware culling profile path based on the switch configuration
	bool lockHardwareStencilStack = false;
	if (reconstructStencilOnlyOnTrig)
	{
		// Trigger Mode: Push registers only if lights are processing actively
		// Replaced missing mLightCount with dynamic items count tracking cleanly
		lockHardwareStencilStack = (useDualPassAlphaGate && gl_lights && di->drawlists[GLDL_PLAINWALLS].drawitems.Size() > 0);
	}
	else
	{
		// Continuous Mode: GPU push/pop cycles inside the render loop
		lockHardwareStencilStack = useDualPassAlphaGate;
	}

	if (lockHardwareStencilStack)
	{
		// --- [Hardware Stencil Attribute Push] ---
		glPushAttrib(GL_STENCIL_BUFFER_BIT);

		glEnable(GL_STENCIL_TEST);
		glStencilMask(0x80); // Isolate our alpha masking footprint inside the 8th bit plane
		glClearStencil(0);
		glClear(GL_STENCIL_BUFFER_BIT); // Localized cache cushion wipe

		glStencilFunc(GL_ALWAYS, 0x80, 0x80);
		glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE); // Replace buffer with 0x80 on visible fragments
	}

	switch (drawitems[i].rendertype)
	{
	case DrawType_FLAT:
	{
		HWFlat * f = flats[drawitems[i].index];
		if (f != nullptr)
		{
			RenderFlat.Clock();
			f->DrawGL1xFlatImplementation(nullptr, di, state, currentGL1xPass);
			RenderFlat.Unclock();
		}
		break;
	}
	case DrawType_WALL:
	{
		HWWall * w = walls[drawitems[i].index];
		if (w != nullptr)
		{
			RenderWall.Clock();

			if (useDualPassAlphaGate)
			{
				// --- [Single-Pass Alpha/Z Sync] ---
				glEnable(GL_ALPHA_TEST);
				glAlphaFunc(GL_GREATER, gl_mask_threshold);

				state.SetDepthMask(true); // Open Z-write to cache the wireframe fences natively via state
				state.ApplyFixedGL1xFunction(nullptr);

				w->DrawGL1xWallImplementation(nullptr, di, state, currentGL1xPass); // DRAW ONCE

				glDisable(GL_ALPHA_TEST);
				state.ApplyFixedGL1xFunction(nullptr);
			}
			else
			{
				w->DrawGL1xWallImplementation(nullptr, di, state, currentGL1xPass);
			}
			RenderWall.Unclock();
		}
		break;
	}
	case DrawType_SPRITE:
	{
		HWSprite * s = sprites[drawitems[i].index];
		if (s != nullptr)
		{
			RenderSprite.Clock();
			// Legacy sprites drawing pass fully integrated natively
			s->DrawSprite(di, state, translucent);
			RenderSprite.Unclock();
		}
		break;
	}
	}

	if (lockHardwareStencilStack)
	{
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP); // Lock stencil buffer to read-only state for subsequent light passes
	}

	//--------------------------------------------------------------------------
	// STEP 2:           THE TRANSLUSCENT DYNLIGHT SURFACES
	//--------------------------------------------------------------------------
	// --- [LZDoom07 Lights-Off Portal Lifeline Escape Gate] ---
	//                   Can't configure dynlight intensities here!
	// So in gl_20.cpp, in "gl_SetupLightWall" and "gl_SetupLightFlat" call:
	// "gl_dynlightHandleSpecialLightsLegacy" after "gl_dynlightSaturateLegacy",
	// and also call "gl_dynlightTameBigLightsOnMirroredSurfacesLegacy".
	// ---
	// If the user completely deactivated dynamic lights in the menu, STEP 2 loop 
	// will be skipped entirely. To prevent glPushAttrib from leaking and blinding 
	// native portals, we execute an immediate, clean state restoration right here!
	bool runDynlightLoop = false;
	if (reconstructStencilOnlyOnTrig)
	{
		// [Darkcrafter07] - Replaced missing mLightCount with active drawitems size tracking natively
		runDynlightLoop = (gl.legacyMode && translucent &&
			currentRenderType != DrawType_SPRITE && gl_lights && di->drawlists[GLDL_PLAINWALLS].drawitems.Size() > 0);
	}
	else
	{
		runDynlightLoop = (gl.legacyMode && translucent &&
			currentRenderType != DrawType_SPRITE && di->drawlists[GLDL_PLAINWALLS].drawitems.Size() > 0);
	}

	if (runDynlightLoop)
	{
		if (currentRenderType == DrawType_FLAT || currentRenderType == DrawType_WALL)
		{
			// TOTAL DISTANCE FOG BOUNDARY GEOMETRY CULLING FILTER
			// If this wall primitive is a fake fog boundary line,
			// SKIP skip lightmap projections over it on ANY distance.
			// Since fake fog lines share identical 3D coordinates with solid room seams,
			// culling them here permanently cures the legacy 16-bit Z-buffer depth fighting,
			// completely extinguishing far-away flickering and sector side alternating artifacts
			if (currentRenderType == DrawType_WALL && walls[index] != nullptr && walls[index]->type == RENDERWALL_FOGBOUNDARY)
			{
				if (lockHardwareStencilStack)
				{
					glStencilMask(0x80);
					glClearStencil(0);
					glClear(GL_STENCIL_BUFFER_BIT);
					glPopAttrib();
				}
				g_isCurrentlyHWWallDrawing = nullptr; // Clear trace tracker safely right before bypass return
				return; // Safe and clean bypass on any distance before changing registers
			}

			if (gl_SetupLightTexture())
			{
				OpenGLRenderer::gl_RenderState.g_isCurrentlyGL1xDynlightPassActive = true;
				OpenGLRenderer::gl_RenderState.g_isCurrentlyDrawingGL1xTransluscentDynlightWall = true;
				OpenGLRenderer::gl_RenderState.g_isCurrentlyDrawingGL1xTransluscentDynlightFlat = true;

				OpenGLRenderer::gl_RenderState.EnableFog(false);
				OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);

				glDisable(GL_FOG);
				glDepthFunc(GL_LEQUAL);
				glDepthMask(false);

				// --- DETECT CUMULATIVE TRANSLUCENCY ---
				// Check overdraw heights strictly within the current sector bounds,
				// minimizing the stacked overexposed cumulative translucency overlay.
				// Enhanced to natively support deep layered 3D-water and volumetric fog.
				// Instead of an early break on the first boundary hit, it evaluates the 
				// full spatial continuum of flat planes within the target sector memory page.
				bool maskColorChannelsOut = false;
				if (currentRenderType == DrawType_FLAT)
				{
					// Natively resolve maptime tick straight from the per-viewpoint Level locals link
					const int currentMapTimeTick = di->Level->maptime;

					// BRANCH A: CACHED WAY
					if (useDoDrawTranslucCumulDetectCache)
					{
						FTranslucencyGL1xCacheEntry& entry = g_TranslucencyGL1xOverdrawCache[index];
						if (entry.lastUpdateTick != -1 && (currentMapTimeTick - entry.lastUpdateTick) < 8)
						{
							maskColorChannelsOut = entry.maskColorChannelsOut;
						}
						else
						{
							if (index >= 0 && index < (int)flats.Size())
							{
								HWFlat* f = flats[index];
								if (f != nullptr && (uintptr_t)f > 0x10000 && f->sector != nullptr && (uintptr_t)f->sector > 0x10000)
								{
									float currentFlatZ = (float)f->z;
									float viewerZ = (float)r_viewpoint.Pos.Z;
									bool isViewerAbove = (viewerZ > currentFlatZ);
									void* targetSectorPtr = f->sector;
									int activeOverdrawLayersCount = 0;

									for (unsigned int j = 0; j < flats.Size(); j++)
									{
										if (flats[j] != nullptr && flats[j]->sector != nullptr && (uintptr_t)flats[j]->sector > 0x10000 && flats[j]->sector == targetSectorPtr)
										{
											float checkFlatZ = (float)flats[j]->z;
											if (isViewerAbove)
											{
												if (checkFlatZ > currentFlatZ && viewerZ > checkFlatZ) activeOverdrawLayersCount++;
											}
											else
											{
												if (checkFlatZ < currentFlatZ && viewerZ < checkFlatZ) activeOverdrawLayersCount++;
											}
										}
									}
									if (activeOverdrawLayersCount > 0) maskColorChannelsOut = true;
								}
							}
							entry.maskColorChannelsOut = maskColorChannelsOut;
							entry.lastUpdateTick = currentMapTimeTick;
						}
					}
					// BRANCH B: UNCACHED WAY
					else
					{
						if (index >= 0 && index < (int)flats.Size())
						{
							HWFlat* f = flats[index];
							if (f != nullptr && (uintptr_t)f > 0x10000 && f->sector != nullptr && (uintptr_t)f->sector > 0x10000)
							{
								float currentFlatZ = (float)f->z;
								float viewerZ = (float)r_viewpoint.Pos.Z;
								bool isViewerAbove = (viewerZ > currentFlatZ);
								void* targetSectorPtr = f->sector;
								int activeOverdrawLayersCount = 0;

								for (unsigned int j = 0; j < flats.Size(); j++)
								{
									if (flats[j] != nullptr && flats[j]->sector != nullptr && (uintptr_t)flats[j]->sector > 0x10000 && flats[j]->sector == targetSectorPtr)
									{
										float checkFlatZ = (float)flats[j]->z;
										if (isViewerAbove)
										{
											if (checkFlatZ > currentFlatZ && viewerZ > checkFlatZ) activeOverdrawLayersCount++;
										}
										else
										{
											if (checkFlatZ < currentFlatZ && viewerZ < checkFlatZ) activeOverdrawLayersCount++;
										}
									}
								}
								if (activeOverdrawLayersCount > 0) maskColorChannelsOut = true;
							}
						}
					}
				}

				if (maskColorChannelsOut)
				{
					state.SetColorMask(false, false, false, false);
				}

				if (lockHardwareStencilStack)
				{
					glEnable(GL_STENCIL_TEST);
					glStencilFunc(GL_EQUAL, 0x80, 0x80); // Lock lights strictly to our baked 0x80 mask
				}

				// --- PASS 1: REGULAR MODULATED DYNAMIC LIGHTS CHANNEL ---
				glBlendEquation(GL_FUNC_ADD);
				glBlendFunc(GL_DST_COLOR, GL_ONE);
				if (currentRenderType == DrawType_WALL && walls[index] != nullptr)
					walls[index]->DrawGL1xWallImplementation(nullptr, di, state, GLPASS_LIGHTTEX);
				else if (currentRenderType == DrawType_FLAT && flats[index] != nullptr)
					flats[index]->DrawGL1xFlatImplementation(nullptr, di, state, GLPASS_LIGHTTEX);

				// --- PASS 2: SPECIAL ADDITIVE LIGHTS PLACED ON PURPOSE CHANNEL ---
				glBlendEquation(GL_FUNC_ADD);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
				if (currentRenderType == DrawType_WALL && walls[index] != nullptr)
				walls[index]->DrawGL1xWallImplementation(nullptr, di, state, GLPASS_LIGHTTEX_ADDITIVE);
				else if (currentRenderType == DrawType_FLAT && flats[index] != nullptr)
				flats[index]->DrawGL1xFlatImplementation(nullptr, di, state, GLPASS_LIGHTTEX_ADDITIVE);

				// --- PASS 3: SPECIAL SUBTRACTIVE LIGHTS ANTI-ILLUMINATION CHANNEL ---
				glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
				if (currentRenderType == DrawType_WALL && walls[index] != nullptr)
					walls[index]->DrawGL1xWallImplementation(nullptr, di, state, GLPASS_TRANSLUCENT_LIGHTTEX);
				else if (currentRenderType == DrawType_FLAT && flats[index] != nullptr)
					flats[index]->DrawGL1xFlatImplementation(nullptr, di, state, GLPASS_TRANSLUCENT_LIGHTTEX);

				// RECOVERY AND CLEANUP
				if (lockHardwareStencilStack)
				{
					// --- THE LIFELINE CACHE RESET GATE ---
					if (walls[index] != nullptr) walls[index]->vertcount = 0;

					glStencilMask(0x80);
					glClearStencil(0);
					glClear(GL_STENCIL_BUFFER_BIT); // Flash clear bit 0x80 safely

					glPopAttrib(); // Pop and restore original pre-pass stencil register stack loops
				}

				if (maskColorChannelsOut)
				{
					state.SetColorMask(true, true, true, true);
				}

				glBlendEquation(GL_FUNC_ADD);

				glEnable(GL_FOG);
				glDepthMask(false);
				glDepthFunc(GL_LEQUAL); // Restore baseline depth check for subsequent items
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

				OpenGLRenderer::gl_RenderState.EnableFog(true);
				OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);
			}
		}
	}
	else if (!reconstructStencilOnlyOnTrig && useDualPassAlphaGate)
	{
		// Fallback Cleanup for 53 FPS Continuous Mode when dynamic lights are disabled
		if (walls[index] != nullptr) walls[index]->vertcount = 0;
		glStencilMask(0x80);
		glClearStencil(0);
		glClear(GL_STENCIL_BUFFER_BIT);
		glPopAttrib();
		OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);
	}

	OpenGLRenderer::gl_RenderState.g_isCurrentlyDrawingGL1xTransluscentDynlightWall = false;
	OpenGLRenderer::gl_RenderState.g_isCurrentlyDrawingGL1xTransluscentDynlightFlat = false;
}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::Draw(HWDrawInfo *di, FRenderState &state, bool translucent)
{
	for (unsigned i = 0; i < drawitems.Size(); i++)
	{
		DoDraw(di, state, translucent, i);
	}
}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::DrawWalls(HWDrawInfo *di, FRenderState &state, bool translucent)
{
	HWWallDispatcher dis(di);
	RenderWall.Clock();
	for (auto &item : drawitems)
	{
		walls[item.index]->DrawWall(&dis, state, translucent);
	}
	RenderWall.Unclock();
}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::DrawFlats(HWDrawInfo *di, FRenderState &state, bool translucent)
{
	RenderFlat.Clock();
	for (unsigned i = 0; i<drawitems.Size(); i++)
	{
		flats[drawitems[i].index]->DrawFlat(di, state, translucent);
	}
	RenderFlat.Unclock();
}

//==========================================================================
//
//
//
//==========================================================================
//void HWDrawList::DrawGL1xWalls(HWWallDispatcher* dispatcher, HWDrawInfo* di, FRenderState& state, int pass)
//{
//	RenderWall.Clock();
//	for (size_t i = 0; i < drawitems.Size(); i++)
//	{
//		if (walls[drawitems[i].index] != nullptr)
//		{
//			// Seamlessly forward both the dispatcher and raw drawinfo pointers down the pipe!
//			walls[drawitems[i].index]->DrawGL1xWallImplementation(dispatcher, di, state, pass);
//		}
//	}
//	RenderWall.Unclock();
//}

//void HWDrawList::DrawGL1xWalls(HWWallDispatcher* dispatcher, HWDrawInfo* di, FRenderState& state, int pass)
//{
//	RenderWall.Clock();
//
//	if (drawitems.Size() == 0 || drawitems.Data() == nullptr)
//	{
//		RenderWall.Unclock();
//		return;
//	}
//
//	glEnable(GL_TEXTURE_2D);
//
//	// [Darkcrafter07] - THE MONOLITHIC IMMEDIATE MODE WALLS BLITTER!
//	for (size_t i = 0; i < drawitems.Size(); i++)
//	{
//		int wallIdx = drawitems[i].index;
//		if (wallIdx >= 0 && wallIdx < (int)walls.Size())
//		{
//			HWWall* directWall = walls[wallIdx];
//			if (directWall != nullptr && directWall->seg != nullptr)
//			{
//				vertex_t* v1 = directWall->seg->v1;
//				vertex_t* v2 = directWall->seg->v2;
//
//				if (v1 != nullptr && v2 != nullptr)
//				{
//					glBegin(GL_QUADS);
//
//					float r = directWall->lightlevel / 255.0f;
//					glColor4f(r, r, r, directWall->alpha);
//
//					// Stream walls geometry coordinates directly matching textures wraps factors
//					glTexCoord2f(0.0f, 1.0f); glVertex3f((float)v1->p.X, (float)v1->p.Y, directWall->ztop[1]);
//					glTexCoord2f(1.0f, 1.0f); glVertex3f((float)v2->p.X, (float)v2->p.Y, directWall->ztop[1]);
//					glTexCoord2f(1.0f, 0.0f); glVertex3f((float)v2->p.X, (float)v2->p.Y, directWall->zbottom[0]);
//					glTexCoord2f(0.0f, 0.0f); glVertex3f((float)v1->p.X, (float)v1->p.Y, directWall->zbottom[0]);
//
//					glEnd();
//				}
//			}
//		}
//	}
//
//	RenderWall.Unclock();
//}

void HWDrawList::DrawGL1xWalls(HWWallDispatcher* dispatcher, HWDrawInfo* di, FRenderState& state, int pass)
{
	RenderWall.Clock();

	if (drawitems.Size() == 0 || drawitems.Data() == nullptr)
	{
		RenderWall.Unclock();
		return;
	}

	// [Darkcrafter07] - Mirroring the flats breakthrough: read properties straight 
	// from the active walls vector array via safe descriptors mappings!
	for (size_t i = 0; i < drawitems.Size(); i++)
	{
		int wallIdx = drawitems[i].index;

		if (wallIdx >= 0 && wallIdx < (int)walls.Size())
		{
			HWWall* directWall = walls[wallIdx];

			if (directWall != nullptr && directWall->seg != nullptr)
			{
				// Seamlessly forward both the dispatcher and raw drawinfo pointers down the pipe!
				directWall->DrawGL1xWallImplementation(dispatcher, di, state, pass);
			}
		}
	}

	RenderWall.Unclock();
}

//==========================================================================
//
//
//
//==========================================================================
//void HWDrawList::DrawGL1xFlats(HWDrawInfo* di, FRenderState& state, int pass)
//{
//	RenderFlat.Clock();
//	for (size_t i = 0; i < drawitems.Size(); i++)
//	{
//		if (flats[drawitems[i].index] != nullptr) // здесь
//		{
//			// Pass scene context "di" and a current pass
//			flats[drawitems[i].index]->DrawGL1xFlatImplementation(nullptr, di, state, pass);
//		}
//	}
//	RenderFlat.Unclock();
//}

//void HWDrawList::DrawGL1xFlats(HWDrawInfo* di, FRenderState& state, int pass)
//{
//	RenderFlat.Clock();
//
//	if (drawitems.Size() == 0 || drawitems.Data() == nullptr)
//	{
//		RenderFlat.Unclock();
//		return;
//	}
//
//	// Enable standard fixed-function texturing hardware layer
//	glEnable(GL_TEXTURE_2D);
//
//	// [Darkcrafter07] - THE MONOLITHIC IMMEDIATE MODE FLATS BLITTER!
//	for (size_t i = 0; i < drawitems.Size(); i++)
//	{
//		int flatIdx = drawitems[i].index;
//		if (flatIdx >= 0 && flatIdx < (int)flats.Size())
//		{
//			HWFlat* directFlat = flats[flatIdx];
//			if (directFlat != nullptr && directFlat->sector != nullptr)
//			{
//				// Execute traditional hardware polygon streaming block!
//				glBegin(GL_POLYGON);
//
//				// Calculate lighting and color bits from sector properties
//				float r = directFlat->sector->lightlevel / 255.0f;
//				glColor4f(r, r, r, directFlat->alpha);
//
//				float vx = (float)di->gl1xViewX;
//				float vy = (float)di->gl1xViewY;
//
//				// [Darkcrafter07] - FIXED E1776! Instantiated explicit DVector2 positions 
//				// to satisfy the strict deleted fixed-point ZatPoint interfaces!
//				DVector2 pos0(vx - 64.f, vy - 64.f);
//				DVector2 pos1(vx + 64.f, vy - 64.f);
//				DVector2 pos2(vx + 64.f, vy + 64.f);
//				DVector2 pos3(vx - 64.f, vy + 64.f);
//
//				// Extract valid heights streams on the fly via the legal vector signatures down the pipe!
//				glTexCoord2f(0.0f, 0.0f); glVertex3f((float)pos0.X, (float)pos0.Y, (float)directFlat->sector->floorplane.ZatPoint(pos0));
//				glTexCoord2f(1.0f, 0.0f); glVertex3f((float)pos1.X, (float)pos1.Y, (float)directFlat->sector->floorplane.ZatPoint(pos1));
//				glTexCoord2f(1.0f, 1.0f); glVertex3f((float)pos2.X, (float)pos2.Y, (float)directFlat->sector->floorplane.ZatPoint(pos2));
//				glTexCoord2f(0.0f, 1.0f); glVertex3f((float)pos3.X, (float)pos3.Y, (float)directFlat->sector->floorplane.ZatPoint(pos3));
//
//				glEnd();
//			}
//		}
//	}
//
//	RenderFlat.Unclock();
//}

void HWDrawList::DrawGL1xFlats(HWDrawInfo* di, FRenderState& state, int pass)
{
	RenderFlat.Clock();

	// Guard checkpoint: if the dynamic items array is empty, abort pass execution safely
	if (drawitems.Size() == 0 || drawitems.Data() == nullptr)
	{
		RenderFlat.Unclock();
		return;
	}

	// [Darkcrafter07] - THE MONOLITHIC FLAT DUMP TRICK!
	// We read the true index inside our local arrays and extract the aggregated HWFlat* pointer!
	for (size_t i = 0; i < drawitems.Size(); i++)
	{
		// [Darkcrafter07] - Extract the valid index token stored inside the current DrawItem descriptor
		int flatIdx = drawitems[i].index;

		// Secure validation check ensures index stays strictly within allocated array bounds
		if (flatIdx >= 0 && flatIdx < (int)flats.Size())
		{
			HWFlat* directFlat = flats[flatIdx];

			if (directFlat != nullptr && directFlat->sector != nullptr)
			{
				// Seamlessly pass context pointers straight down to the implementation layer!
				directFlat->DrawGL1xFlatImplementation(nullptr, di, state, pass);
			}
		}
	}

	RenderFlat.Unclock();
}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::DrawSorted(HWDrawInfo *di, FRenderState &state, SortNode * head)
{
	float clipsplit[2];
	int relation = 0;
	float z = 0.f;

	state.GetClipSplit(clipsplit);

	if (drawitems[head->itemindex].rendertype == DrawType_FLAT)
	{
		z = flats[drawitems[head->itemindex].index]->z;
		relation = z > di->Viewpoint.Pos.Z ? 1 : -1;
	}


	// left is further away, i.e. for stuff above viewz its z coordinate higher, for stuff below viewz its z coordinate is lower
	if (head->left)
	{
		if (relation == -1)
		{
			state.SetClipSplit(clipsplit[0], z);	// render below: set flat as top clip plane
		}
		else if (relation == 1)
		{
			state.SetClipSplit(z, clipsplit[1]);	// render above: set flat as bottom clip plane
		}
		DrawSorted(di, state, head->left);
		state.SetClipSplit(clipsplit);
	}
	DoDraw(di, state, true, head->itemindex);
	if (head->equal)
	{
		SortNode * ehead = head->equal;
		while (ehead)
		{
			DoDraw(di, state, true, ehead->itemindex);
			ehead = ehead->equal;
		}
	}
	// right is closer, i.e. for stuff above viewz its z coordinate is lower, for stuff below viewz its z coordinate is higher
	if (head->right)
	{
		if (relation == 1)
		{
			state.SetClipSplit(clipsplit[0], z);	// render below: set flat as top clip plane
		}
		else if (relation == -1)
		{
			state.SetClipSplit(z, clipsplit[1]);	// render above: set flat as bottom clip plane
		}
		DrawSorted(di, state, head->right);
		state.SetClipSplit(clipsplit);
	}
}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::DrawSorted(HWDrawInfo *di, FRenderState &state)
{
	if (drawitems.Size() == 0) return;

	if (!sorted)
	{
		screen->mVertexData->Map();
		Sort(di);
		screen->mVertexData->Unmap();
	}
	state.ClearClipSplit();
	state.EnableClipDistance(1, true);
	state.EnableClipDistance(2, true);
	DrawSorted(di, state, sorted);
	state.EnableClipDistance(1, false);
	state.EnableClipDistance(2, false);
	state.ClearClipSplit();
}

