#include "solvespace.h"

int I, N, FLAG;

void SShell::MakeFromUnionOf(SShell *a, SShell *b) {
    MakeFromBoolean(a, b, AS_UNION);
}

void SShell::MakeFromDifferenceOf(SShell *a, SShell *b) {
    MakeFromBoolean(a, b, AS_DIFFERENCE);
}

//-----------------------------------------------------------------------------
// Take our original pwl curve. Wherever an edge intersects a surface within
// either agnstA or agnstB, split the piecewise linear element. Then refine
// the intersection so that it lies on all three relevant surfaces: the
// intersecting surface, srfA, and srfB. (So the pwl curve should lie at
// the intersection of srfA and srfB.) Return a new pwl curve with everything
// split.
//-----------------------------------------------------------------------------
static Vector LineStart, LineDirection;
static int ByTAlongLine(const void *av, const void *bv)
{
    SInter *a = (SInter *)av,
           *b = (SInter *)bv;
    
    double ta = (a->p.Minus(LineStart)).DivPivoting(LineDirection),
           tb = (b->p.Minus(LineStart)).DivPivoting(LineDirection);

    return (ta > tb) ? 1 : -1;
}
SCurve SCurve::MakeCopySplitAgainst(SShell *agnstA, SShell *agnstB,
                                    SSurface *srfA, SSurface *srfB)
{
    SCurve ret;
    ret = *this;
    ZERO(&(ret.pts));

    Vector *p = pts.First();
    if(!p) oops();
    Vector prev = *p;
    ret.pts.Add(p);
    p = pts.NextAfter(p);

    for(; p; p = pts.NextAfter(p)) {
        List<SInter> il;
        ZERO(&il);

        // Find all the intersections with the two passed shells
        if(agnstA) agnstA->AllPointsIntersecting(prev, *p, &il, true,true,true);
        if(agnstB) agnstB->AllPointsIntersecting(prev, *p, &il, true,true,true);

        if(il.n > 0) {
            // The intersections were generated by intersecting the pwl
            // edge against a surface; so they must be refined to lie
            // exactly on the original curve.
            SInter *pi;
            for(pi = il.First(); pi; pi = il.NextAfter(pi)) {
                double u, v;
                (pi->srf)->ClosestPointTo(pi->p, &u, &v, false);
                (pi->srf)->PointOnSurfaces(srfA, srfB, &u, &v);
                pi->p = (pi->srf)->PointAt(u, v);
            }

            // And now sort them in order along the line. Note that we must
            // do that after refining, in case the refining would make two
            // points switch places.
            LineStart = prev;
            LineDirection = p->Minus(prev);
            qsort(il.elem, il.n, sizeof(il.elem[0]), ByTAlongLine);
   
            // And now uses the intersections to generate our split pwl edge(s)
            Vector prev = Vector::From(VERY_POSITIVE, 0, 0);
            for(pi = il.First(); pi; pi = il.NextAfter(pi)) {
                double t = (pi->p.Minus(LineStart)).DivPivoting(LineDirection);
                // On-edge intersection will generate same split point for
                // both surfaces, so don't create zero-length edge.
                if(!prev.Equals(pi->p)) {
                    ret.pts.Add(&(pi->p));
                }
                prev = pi->p;
            }
        }

        il.Clear();
        ret.pts.Add(p);
        prev = *p;
    }
    return ret;
}

void SShell::CopyCurvesSplitAgainst(bool opA, SShell *agnst, SShell *into) {    
    SCurve *sc;
    for(sc = curve.First(); sc; sc = curve.NextAfter(sc)) {
        SCurve scn = sc->MakeCopySplitAgainst(agnst, NULL,
                                surface.FindById(sc->surfA),
                                surface.FindById(sc->surfB));
        scn.source = opA ? SCurve::FROM_A : SCurve::FROM_B;

        hSCurve hsc = into->curve.AddAndAssignId(&scn);
        // And note the new ID so that we can rewrite the trims appropriately
        sc->newH = hsc;
    }
}

void SSurface::TrimFromEdgeList(SEdgeList *el) {
    el->l.ClearTags();
   
    STrimBy stb;
    ZERO(&stb);
    for(;;) {
        // Find an edge, any edge; we'll start from there.
        SEdge *se;
        for(se = el->l.First(); se; se = el->l.NextAfter(se)) {
            if(se->tag) continue;
            break;    
        }
        if(!se) break;
        se->tag = 1;
        stb.start = se->a;
        stb.finish = se->b;
        stb.curve.v = se->auxA;
        stb.backwards = se->auxB ? true : false;

        // Find adjoining edges from the same curve; those should be
        // merged into a single trim.
        bool merged;
        do {
            merged = false;
            for(se = el->l.First(); se; se = el->l.NextAfter(se)) {
                if(se->tag)                         continue;
                if(se->auxA != stb.curve.v)         continue;
                if(( se->auxB && !stb.backwards) ||
                   (!se->auxB &&  stb.backwards))   continue;

                if((se->a).Equals(stb.finish)) {
                    stb.finish = se->b;
                    se->tag = 1;
                    merged = true;
                } else if((se->b).Equals(stb.start)) {
                    stb.start = se->a;
                    se->tag = 1;
                    merged = true;
                }
            }
        } while(merged);

        // And add the merged trim, with xyz (not uv like the polygon) pts
        stb.start  = PointAt(stb.start.x,  stb.start.y);
        stb.finish = PointAt(stb.finish.x, stb.finish.y);
        trim.Add(&stb);
    }
}

// For each edge, we record the membership of the regions to its left and
// right, which we call the "in direction" and "out direction" (wrt its
// outer normal)
#define INDIR   (0)
#define OUTDIR  (8)
// Regions of interest are the other shell itself, the coincident faces of the
// shell (same or opposite normal) and the original surface.
#define SHELL                   (0)
#define COINCIDENT_SAME         (1)
#define COINCIDENT_OPPOSITE     (2)
#define ORIG                    (3)
// Macro for building bit to test
#define INSIDE(reg, dir) (1 << ((reg)+(dir)))

static bool KeepRegion(int type, bool opA, int tag, int dir)
{
    bool inShell = (tag & INSIDE(SHELL, dir))                != 0,
         inSame  = (tag & INSIDE(COINCIDENT_SAME, dir))      != 0,
         inOpp   = (tag & INSIDE(COINCIDENT_OPPOSITE, dir))  != 0,
         inOrig  = (tag & INSIDE(ORIG, dir))                 != 0;

    bool inFace = inSame || inOpp;

    // If these are correct, then they should be independent of inShell
    // if inFace is true.
    if(!inOrig) return false;
    switch(type) {
        case SShell::AS_UNION:
            if(opA) {
                return (!inShell && !inFace);
            } else {
                return (!inShell && !inFace) || inSame;
            }
            break;

        case SShell::AS_DIFFERENCE:
            if(opA) {
                return (!inShell && !inFace);
            } else {
                return (inShell && !inFace) || inSame;
            }
            break;

        default: oops();
    }
}
static bool KeepEdge(int type, bool opA, int tag)
{
    bool keepIn  = KeepRegion(type, opA, tag, INDIR),
         keepOut = KeepRegion(type, opA, tag, OUTDIR);

    // If the regions to the left and right of this edge are both in or both
    // out, then this edge is not useful and should be discarded.
    if(keepIn && !keepOut) return true;
    return false;
}

static int TagByClassifiedEdge(int bspclass, int reg)
{
    switch(bspclass) {
        case SBspUv::INSIDE:
            return INSIDE(reg, OUTDIR) | INSIDE(reg, INDIR);

        case SBspUv::OUTSIDE:
            return 0;

        case SBspUv::EDGE_PARALLEL:
            return INSIDE(reg, INDIR);

        case SBspUv::EDGE_ANTIPARALLEL:
            return INSIDE(reg, OUTDIR);

        default: oops();
    }
}

void DBPEDGE(int tag) {
    dbp("edge: indir %s %s %s %s ; outdir %s %s %s %s",
        (tag & INSIDE(SHELL, INDIR)) ? "shell" : "",
        (tag & INSIDE(COINCIDENT_SAME, INDIR)) ? "coinc-same" : "",
        (tag & INSIDE(COINCIDENT_OPPOSITE, INDIR)) ? "coinc-opp" : "",
        (tag & INSIDE(ORIG, INDIR)) ? "orig" : "",
        (tag & INSIDE(SHELL, OUTDIR)) ? "shell" : "",
        (tag & INSIDE(COINCIDENT_SAME, OUTDIR)) ? "coinc-same" : "",
        (tag & INSIDE(COINCIDENT_OPPOSITE, OUTDIR)) ? "coinc-opp" : "",
        (tag & INSIDE(ORIG, OUTDIR)) ? "orig" : "");
}

void DEBUGEDGELIST(SEdgeList *sel, SSurface *surf) {
    dbp("print %d edges", sel->l.n);
    SEdge *se;
    for(se = sel->l.First(); se; se = sel->l.NextAfter(se)) {
        Vector mid = (se->a).Plus(se->b).ScaledBy(0.5);
        Vector arrow = (se->b).Minus(se->a);
        SWAP(double, arrow.x, arrow.y);
        arrow.x *= -1;
        arrow = arrow.WithMagnitude(0.03);
        arrow = arrow.Plus(mid);

        SS.nakedEdges.AddEdge(surf->PointAt(se->a.x, se->a.y),
                              surf->PointAt(se->b.x, se->b.y));
        SS.nakedEdges.AddEdge(surf->PointAt(mid.x, mid.y),
                              surf->PointAt(arrow.x, arrow.y));
    }
}

//-----------------------------------------------------------------------------
// Trim this surface against the specified shell, in the way that's appropriate
// for the specified Boolean operation type (and which operand we are). We
// also need a pointer to the shell that contains our own surface, since that
// contains our original trim curves.
//-----------------------------------------------------------------------------
SSurface SSurface::MakeCopyTrimAgainst(SShell *agnst, SShell *parent,
                                       SShell *into,
                                       int type, bool opA)
{
    SSurface ret;
    // The returned surface is identical, just the trim curves change
    ret = *this;
    ZERO(&(ret.trim));

    // First, build a list of the existing trim curves; update them to use
    // the split curves.
    STrimBy *stb;
    for(stb = trim.First(); stb; stb = trim.NextAfter(stb)) {
        STrimBy stn = *stb;
        stn.curve = (parent->curve.FindById(stn.curve))->newH;
        ret.trim.Add(&stn);
    }

    if(type == SShell::AS_DIFFERENCE && !opA) {
        // The second operand of a Boolean difference gets turned inside out
        ret.Reverse();
    }

    // Build up our original trim polygon; remember the coordinates could
    // be changed if we just flipped the surface normal.
    SEdgeList orig;
    ZERO(&orig);
    ret.MakeEdgesInto(into, &orig, true);
    ret.trim.Clear();
    // which means that we can't necessarily use the old BSP...
    SBspUv *origBsp = SBspUv::From(&orig);

    // Find any surfaces from the other shell that are coincident with ours,
    // and get the intersection polygons, grouping surfaces with the same
    // and with opposite normal separately.
    SEdgeList sameNormal, oppositeNormal;
    ZERO(&sameNormal);
    ZERO(&oppositeNormal);
    agnst->MakeCoincidentEdgesInto(&ret, true, &sameNormal, into);
    agnst->MakeCoincidentEdgesInto(&ret, false, &oppositeNormal, into);
    // and cull parallel or anti-parallel pairs, which may occur if multiple
    // surfaces are coincident with ours
    sameNormal.CullExtraneousEdges();
    oppositeNormal.CullExtraneousEdges();
    // and build the trees for quick in-polygon testing
    SBspUv *sameBsp = SBspUv::From(&sameNormal);
    SBspUv *oppositeBsp = SBspUv::From(&oppositeNormal);

    // And now intersect the other shell against us
    SEdgeList inter;
    ZERO(&inter);

    SSurface *ss;
    for(ss = agnst->surface.First(); ss; ss = agnst->surface.NextAfter(ss)) {
        SCurve *sc;
        for(sc = into->curve.First(); sc; sc = into->curve.NextAfter(sc)) {
            if(sc->source != SCurve::FROM_INTERSECTION) continue;
            if(opA) {
                if(sc->surfA.v != h.v || sc->surfB.v != ss->h.v) continue;
            } else {
                if(sc->surfB.v != h.v || sc->surfA.v != ss->h.v) continue;
            }
           
            int i;
            for(i = 1; i < sc->pts.n; i++) {
                Vector a = sc->pts.elem[i-1],
                       b = sc->pts.elem[i];

                Point2d auv, buv;
                ss->ClosestPointTo(a, &(auv.x), &(auv.y));
                ss->ClosestPointTo(b, &(buv.x), &(buv.y));

                int c = ss->bsp->ClassifyEdge(auv, buv);
                if(c != SBspUv::OUTSIDE) {
                    Vector ta = Vector::From(0, 0, 0);
                    Vector tb = Vector::From(0, 0, 0);
                    ret.ClosestPointTo(a, &(ta.x), &(ta.y));
                    ret.ClosestPointTo(b, &(tb.x), &(tb.y));

                    Vector tn = ret.NormalAt(ta.x, ta.y);
                    Vector sn = ss->NormalAt(auv.x, auv.y);

                    // We are subtracting the portion of our surface that
                    // lies in the shell, so the in-plane edge normal should
                    // point opposite to the surface normal.
                    bool bkwds = true;
                    if((tn.Cross(b.Minus(a))).Dot(sn) < 0) bkwds = !bkwds;
                    if(type == SShell::AS_DIFFERENCE && !opA) bkwds = !bkwds;
                    if(bkwds) {
                        inter.AddEdge(tb, ta, sc->h.v, 1);
                    } else { 
                        inter.AddEdge(ta, tb, sc->h.v, 0);
                    }
                }
            }
        }
    }

    SEdgeList final;
    ZERO(&final);

    SEdge *se;
    for(se = orig.l.First(); se; se = orig.l.NextAfter(se)) {
        Point2d auv = (se->a).ProjectXy(),
                buv = (se->b).ProjectXy();

        int c_same = sameBsp->ClassifyEdge(auv, buv);
        int c_opp = oppositeBsp->ClassifyEdge(auv, buv);

        // Get the midpoint of this edge
        Point2d am = (auv.Plus(buv)).ScaledBy(0.5);
        Vector pt = ret.PointAt(am.x, am.y);
        // and the outer normal from the trim polygon (within the surface)
        Vector surf_n = ret.NormalAt(am.x, am.y);
        Vector ea = ret.PointAt(auv.x, auv.y),
               eb = ret.PointAt(buv.x, buv.y);
        Vector edge_n = surf_n.Cross((eb.Minus(ea)));

        int c_shell = agnst->ClassifyPoint(pt, edge_n, surf_n);

        int tag = 0;
        tag |= INSIDE(ORIG, INDIR);
        tag |= TagByClassifiedEdge(c_same, COINCIDENT_SAME);
        tag |= TagByClassifiedEdge(c_opp,  COINCIDENT_OPPOSITE);

        if(c_shell == SShell::INSIDE) {
            tag |= INSIDE(SHELL, INDIR) | INSIDE(SHELL, OUTDIR);
        } else if(c_shell == SShell::OUTSIDE) {
            tag |= 0;
        } else if(c_shell == SShell::SURF_PARALLEL) {
            tag |= INSIDE(SHELL, INDIR);
        } else if(c_shell == SShell::SURF_ANTIPARALLEL) {
            tag |= INSIDE(SHELL, OUTDIR);
        } else if(c_shell == SShell::EDGE_PARALLEL) {
            tag |= INSIDE(SHELL, INDIR);
        } else if(c_shell == SShell::EDGE_ANTIPARALLEL) {
            tag |= INSIDE(SHELL, OUTDIR);
        } else if(c_shell == SShell::EDGE_TANGENT) {
            continue;
        }

        if(KeepEdge(type, opA, tag)) {
            final.AddEdge(se->a, se->b, se->auxA, se->auxB);
        }
    }

    for(se = inter.l.First(); se; se = inter.l.NextAfter(se)) {
        Point2d auv = (se->a).ProjectXy(),
                buv = (se->b).ProjectXy();

        int c_this = origBsp->ClassifyEdge(auv, buv);
        int c_same = sameBsp->ClassifyEdge(auv, buv);
        int c_opp = oppositeBsp->ClassifyEdge(auv, buv);

        int tag = 0;
        tag |= TagByClassifiedEdge(c_this, ORIG);
        tag |= TagByClassifiedEdge(c_same, COINCIDENT_SAME);
        tag |= TagByClassifiedEdge(c_opp,  COINCIDENT_OPPOSITE);

        if(type == SShell::AS_DIFFERENCE && !opA) {
            // The second operand of a difference gets turned inside out
            tag |= INSIDE(SHELL, INDIR);
        } else {
            tag |= INSIDE(SHELL, OUTDIR);
        }

        if(KeepEdge(type, opA, tag)) {
            final.AddEdge(se->a, se->b, se->auxA, se->auxB);
        }
    }

    // Cull extraneous edges; duplicates or anti-parallel pairs. In particular,
    // we can get duplicate edges if our surface intersects the other shell
    // at an edge, so that both surfaces intersect coincident (and both
    // generate an intersection edge).
    final.CullExtraneousEdges();

    // Use our reassembled edges to trim the new surface.
    ret.TrimFromEdgeList(&final);

    SPolygon poly;
    ZERO(&poly);
    final.l.ClearTags();
    if(!final.AssemblePolygon(&poly, NULL, true)) {
        DEBUGEDGELIST(&final, &ret);
    }
    poly.Clear();

    sameNormal.Clear();
    oppositeNormal.Clear();
    final.Clear();
    inter.Clear();
    orig.Clear();
    return ret;
}

void SShell::CopySurfacesTrimAgainst(SShell *against, SShell *into,
                                     int type, bool opA)
{
    SSurface *ss;
    for(ss = surface.First(); ss; ss = surface.NextAfter(ss)) {
        SSurface ssn;
        ssn = ss->MakeCopyTrimAgainst(against, this, into, type, opA);
        ss->newH = into->surface.AddAndAssignId(&ssn);
        I++;
    }
}

void SShell::MakeIntersectionCurvesAgainst(SShell *agnst, SShell *into) {
    SSurface *sa;
    for(sa = surface.First(); sa; sa = surface.NextAfter(sa)) {
        SSurface *sb;
        for(sb = agnst->surface.First(); sb; sb = agnst->surface.NextAfter(sb)){
            // Intersect every surface from our shell against every surface
            // from agnst; this will add zero or more curves to the curve
            // list for into.
            sa->IntersectAgainst(sb, this, agnst, into);
        }
        FLAG++;
    }
}

void SShell::CleanupAfterBoolean(void) {
    SSurface *ss;
    for(ss = surface.First(); ss; ss = surface.NextAfter(ss)) {
    }
}

void SShell::MakeFromBoolean(SShell *a, SShell *b, int type) {
    a->MakeClassifyingBsps();
    b->MakeClassifyingBsps();

    // Copy over all the original curves, splitting them so that a
    // piecwise linear segment never crosses a surface from the other
    // shell.
    a->CopyCurvesSplitAgainst(true,  b, this);
    b->CopyCurvesSplitAgainst(false, a, this);

    // Generate the intersection curves for each surface in A against all
    // the surfaces in B (which is all of the intersection curves).
    a->MakeIntersectionCurvesAgainst(b, this);
    
    if(b->surface.n == 0 || a->surface.n == 0) {
        // Then trim and copy the surfaces
        a->CopySurfacesTrimAgainst(b, this, type, true);
        b->CopySurfacesTrimAgainst(a, this, type, false);
    } else {
        a->CopySurfacesTrimAgainst(b, this, type, true);
        b->CopySurfacesTrimAgainst(a, this, type, false);
    }

    // Now that we've copied the surfaces, we know their new hSurfaces, so
    // rewrite the curves to refer to the surfaces by their handles in the
    // result.
    SCurve *sc;
    for(sc = curve.First(); sc; sc = curve.NextAfter(sc)) {
        if(sc->source == SCurve::FROM_A) {
            sc->surfA = a->surface.FindById(sc->surfA)->newH;
            sc->surfB = a->surface.FindById(sc->surfB)->newH;
        } else if(sc->source == SCurve::FROM_B) {
            sc->surfA = b->surface.FindById(sc->surfA)->newH;
            sc->surfB = b->surface.FindById(sc->surfB)->newH;
        } else if(sc->source == SCurve::FROM_INTERSECTION) {
            sc->surfA = a->surface.FindById(sc->surfA)->newH;
            sc->surfB = b->surface.FindById(sc->surfB)->newH;
        } else {
            oops();
        }
    }

    // And clean up the piecewise linear things we made as a calculation aid
    a->CleanupAfterBoolean();
    b->CleanupAfterBoolean();
}

//-----------------------------------------------------------------------------
// All of the BSP routines that we use to perform and accelerate polygon ops.
//-----------------------------------------------------------------------------
void SShell::MakeClassifyingBsps(void) {
    SSurface *ss;
    for(ss = surface.First(); ss; ss = surface.NextAfter(ss)) {
        ss->MakeClassifyingBsp(this);
    }
}

void SSurface::MakeClassifyingBsp(SShell *shell) {
    SEdgeList el;
    ZERO(&el);

    MakeEdgesInto(shell, &el, true);
    bsp = SBspUv::From(&el);

    el.Clear();
}

SBspUv *SBspUv::Alloc(void) {
    return (SBspUv *)AllocTemporary(sizeof(SBspUv));
}

static int ByLength(const void *av, const void *bv)
{
    SEdge *a = (SEdge *)av,
          *b = (SEdge *)bv;
    
    double la = (a->a).Minus(a->b).Magnitude(),
           lb = (b->a).Minus(b->b).Magnitude();

    // Sort in descending order, longest first. This improves numerical
    // stability for the normals.
    return (la < lb) ? 1 : -1;
}
SBspUv *SBspUv::From(SEdgeList *el) {
    SEdgeList work;
    ZERO(&work);

    SEdge *se;
    for(se = el->l.First(); se; se = el->l.NextAfter(se)) {
        work.AddEdge(se->a, se->b, se->auxA, se->auxB);
    }
    qsort(work.l.elem, work.l.n, sizeof(work.l.elem[0]), ByLength);

    SBspUv *bsp = NULL;
    for(se = work.l.First(); se; se = work.l.NextAfter(se)) {
        bsp = bsp->InsertEdge((se->a).ProjectXy(), (se->b).ProjectXy());
    }

    work.Clear();
    return bsp;
}

SBspUv *SBspUv::InsertEdge(Point2d ea, Point2d eb) {
    if(!this) {
        SBspUv *ret = Alloc();
        ret->a = ea;
        ret->b = eb;
        return ret;
    }

    Point2d n = ((b.Minus(a)).Normal()).WithMagnitude(1);
    double d = a.Dot(n);

    double dea = ea.Dot(n) - d,
           deb = eb.Dot(n) - d;

    if(fabs(dea) < LENGTH_EPS && fabs(deb) < LENGTH_EPS) {
        // Line segment is coincident with this one, store in same node
        SBspUv *m = Alloc();
        m->a = ea;
        m->b = eb;
        m->more = more;
        more = m;
    } else if(fabs(dea) < LENGTH_EPS) {
        // Point A lies on this lie, but point B does not
        if(deb > 0) {
            pos = pos->InsertEdge(ea, eb);
        } else {
            neg = neg->InsertEdge(ea, eb);
        }
    } else if(fabs(deb) < LENGTH_EPS) {
        // Point B lies on this lie, but point A does not
        if(dea > 0) {
            pos = pos->InsertEdge(ea, eb);
        } else {
            neg = neg->InsertEdge(ea, eb);
        }
    } else if(dea > 0 && deb > 0) {
        pos = pos->InsertEdge(ea, eb);
    } else if(dea < 0 && deb < 0) {
        neg = neg->InsertEdge(ea, eb);
    } else {
        // New edge crosses this one; we need to split.
        double t = (d - n.Dot(ea)) / (n.Dot(eb.Minus(ea)));
        Point2d pi = ea.Plus((eb.Minus(ea)).ScaledBy(t));
        if(dea > 0) {
            pos = pos->InsertEdge(ea, pi);
            neg = neg->InsertEdge(pi, eb);
        } else {
            neg = neg->InsertEdge(ea, pi);
            pos = pos->InsertEdge(pi, eb);
        }
    }
    return this;
}

int SBspUv::ClassifyPoint(Point2d p, Point2d eb) {
    if(!this) return OUTSIDE;

    Point2d n = ((b.Minus(a)).Normal()).WithMagnitude(1);
    double d = a.Dot(n);

    double dp = p.Dot(n) - d;

    if(fabs(dp) < LENGTH_EPS) {
        SBspUv *f = this;
        while(f) {
            Point2d ba = (f->b).Minus(f->a);
            if(p.DistanceToLine(f->a, ba, true) < LENGTH_EPS) {
                if(eb.DistanceToLine(f->a, ba, false) < LENGTH_EPS) {
                    if(ba.Dot(eb.Minus(p)) > 0) {
                        return EDGE_PARALLEL;
                    } else {
                        return EDGE_ANTIPARALLEL;
                    }
                } else {
                    return EDGE_OTHER;
                }
            }
            f = f->more;
        }
        // Pick arbitrarily which side to send it down, doesn't matter
        int c1 =  neg ? neg->ClassifyPoint(p, eb) : OUTSIDE;
        int c2 =  pos ? pos->ClassifyPoint(p, eb) : INSIDE;
        if(c1 != c2) {
            dbp("MISMATCH: %d %d %08x %08x", c1, c2, neg, pos);
        }
        return c1;
    } else if(dp > 0) {
        return pos ? pos->ClassifyPoint(p, eb) : INSIDE;
    } else {
        return neg ? neg->ClassifyPoint(p, eb) : OUTSIDE;
    }
}

int SBspUv::ClassifyEdge(Point2d ea, Point2d eb) {
    int ret = ClassifyPoint((ea.Plus(eb)).ScaledBy(0.5), eb);
    if(ret == EDGE_OTHER) {
        // Perhaps the edge is tangent at its midpoint (and we screwed up
        // somewhere earlier and failed to split it); try a different
        // point on the edge.
        ret = ClassifyPoint(ea.Plus((eb.Minus(ea)).ScaledBy(0.294)), eb);
    }
    return ret;
}
