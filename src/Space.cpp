#include "libs.h"
#include "Space.h"
#include "Body.h"
#include "Frame.h"
#include "Star.h"
#include "Planet.h"
#include <algorithm>
#include <functional>
#include "Pi.h"
#include "Player.h"
#include "StarSystem.h"
#include "SpaceStation.h"
#include "sbre/sbre.h"
#include "Serializer.h"
#include "collider/collider.h"
#include "pirates.h"
#include "Sfx.h"
#include "Missile.h"

namespace Space {

std::list<Body*> bodies;
Frame *rootFrame;
static void MoveOrbitingObjectFrames(Frame *f);
static void UpdateFramesOfReference();
static void CollideFrame(Frame *f);
static void PruneCorpses();
static void ApplyGravity();
static std::list<Body*> corpses;
static SBodyPath *hyperspacingTo;
static float hyperspaceAnim;

void Init()
{
	rootFrame = new Frame(NULL, "System");
	rootFrame->SetRadius(FLT_MAX);
}

void Clear()
{
	for (std::list<Body*>::iterator i = bodies.begin(); i != bodies.end(); ++i) {
		(*i)->SetFrame(NULL);
		if ((*i) != (Body*)Pi::player) {
			KillBody(*i);
		}
	}
	PruneCorpses();

	Pi::player->SetFrame(rootFrame);
	for (std::list<Frame*>::iterator i = rootFrame->m_children.begin(); i != rootFrame->m_children.end(); ++i) delete *i;
	rootFrame->m_children.clear();
	rootFrame->m_astroBody = 0;
	rootFrame->m_sbody = 0;
}

void RadiusDamage(Body *attacker, Frame *f, const vector3d &pos, double radius, double kgDamage)
{
	for (std::list<Body*>::iterator i = bodies.begin(); i != bodies.end(); ++i) {
		if ((*i)->GetFrame() != f) continue;
		double dist = ((*i)->GetPosition() - pos).Length();
		if (dist < radius) {
			// linear damage decay with distance
			(*i)->OnDamage(attacker, kgDamage * (radius - dist) / radius);
		}
	}
}

void DoECM(const Frame *f, const vector3d &pos, int power_val)
{
	const float ECM_RADIUS = 4000.0f;
	for (std::list<Body*>::iterator i = bodies.begin(); i != bodies.end(); ++i) {
		if ((*i)->GetFrame() != f) continue;
		if (!(*i)->IsType(Object::MISSILE)) continue;

		double dist = ((*i)->GetPosition() - pos).Length();
		if (dist < ECM_RADIUS) {
			// increasing chance of destroying it with proximity
			if (Pi::rng.Double() > (dist / ECM_RADIUS)) {
				static_cast<Missile*>(*i)->ECMAttack(power_val);
			}
		}
	}

}

void Serialize()
{
	using namespace Serializer::Write;
	Serializer::IndexFrames();
	Serializer::IndexBodies();
	Serializer::IndexSystemBodies(Pi::currentSystem);
	Frame::Serialize(rootFrame);
	wr_int(bodies.size());
	for (bodiesIter_t i = bodies.begin(); i != bodies.end(); ++i) {
		//printf("Serializing %s\n", (*i)->GetLabel().c_str());
		(*i)->Serialize();
	}
	if (hyperspacingTo == 0) {
		wr_byte(0);
	} else {
		wr_byte(1);
		hyperspacingTo->Serialize();
		wr_float(hyperspaceAnim);
	}
}

void Unserialize()
{
	using namespace Serializer::Read;
	Serializer::IndexSystemBodies(Pi::currentSystem);
	rootFrame = Frame::Unserialize(0);
	Serializer::IndexFrames();
	int num_bodies = rd_int();
	//printf("%d bodies to read\n", num_bodies);
	for (int i=0; i<num_bodies; i++) {
		Body *b = Body::Unserialize();
		if (b) bodies.push_back(b);
	}
	hyperspaceAnim = 0;
	if (rd_byte()) {
		hyperspacingTo = new SBodyPath;
		SBodyPath::Unserialize(hyperspacingTo);
		hyperspaceAnim = rd_float();
	}
	// bodies with references to others must fix these up
	Serializer::IndexBodies();
	for (bodiesIter_t i = bodies.begin(); i != bodies.end(); ++i) {
		(*i)->PostLoadFixup();
	}
	Frame::PostUnserializeFixup(rootFrame);
}

static Frame *find_frame_with_sbody(Frame *f, const SBody *b)
{
	if (f->m_sbody == b) return f;
	else {
		for (std::list<Frame*>::iterator i = f->m_children.begin();
			i != f->m_children.end(); ++i) {
			
			Frame *found = find_frame_with_sbody(*i, b);
			if (found) return found;
		}
	}
	return 0;
}

Frame *GetFrameWithSBody(const SBody *b)
{
	return find_frame_with_sbody(rootFrame, b);
}

void MoveOrbitingObjectFrames(Frame *f)
{
	if (f == Space::rootFrame) {
		f->SetPosition(vector3d(0,0,0));
		f->SetVelocity(vector3d(0,0,0));
	} else if (f->m_sbody) {
		// this isn't very smegging efficient
		vector3d pos = f->m_sbody->orbit.OrbitalPosAtTime(Pi::GetGameTime());
		vector3d pos2 = f->m_sbody->orbit.OrbitalPosAtTime(Pi::GetGameTime()+1.0);
		vector3d vel = pos2 - pos;
		f->SetPosition(pos);
		f->SetVelocity(vel);
	}
	f->RotateInTimestep(Pi::GetTimeStep());

	for (std::list<Frame*>::iterator i = f->m_children.begin(); i != f->m_children.end(); ++i) {
		MoveOrbitingObjectFrames(*i);
	}
}
static Frame *MakeFrameFor(SBody *sbody, Body *b, Frame *f)
{
	Frame *orbFrame, *rotFrame;
	double frameRadius;

	if (!sbody->parent) {
		if (b) b->SetFrame(f);
		f->m_sbody = sbody;
		f->m_astroBody = b;
		return f;
	}

	if (sbody->type == SBody::TYPE_GRAVPOINT) {
		orbFrame = new Frame(f, sbody->name.c_str());
		orbFrame->m_sbody = sbody;
		orbFrame->m_astroBody = b;
		orbFrame->SetRadius(sbody->GetMaxChildOrbitalDistance()*1.1);
		return orbFrame;
	}

	SBody::BodySuperType supertype = sbody->GetSuperType();

	if ((supertype == SBody::SUPERTYPE_GAS_GIANT) ||
	    (supertype == SBody::SUPERTYPE_ROCKY_PLANET)) {
		// for planets we want an non-rotating frame for a few radii
		// and a rotating frame in the same position but with maybe 1.1*radius,
		// which actually contains the object.
		frameRadius = sbody->GetMaxChildOrbitalDistance()*1.1;
		orbFrame = new Frame(f, sbody->name.c_str());
		orbFrame->m_sbody = sbody;
		orbFrame->SetRadius(frameRadius ? frameRadius : 10*sbody->GetRadius());
	
		assert(sbody->GetRotationPeriod() != 0);
		rotFrame = new Frame(orbFrame, sbody->name.c_str());
		rotFrame->SetRadius(1.1*sbody->GetRadius());
		rotFrame->SetAngVelocity(vector3d(0,2*M_PI/sbody->GetRotationPeriod(),0));
		rotFrame->m_astroBody = b;
		b->SetFrame(rotFrame);
		return orbFrame;
	}
	else if (supertype == SBody::SUPERTYPE_STAR) {
		// stars want a single small non-rotating frame
		orbFrame = new Frame(f, sbody->name.c_str());
		orbFrame->m_sbody = sbody;
		orbFrame->m_astroBody = b;
		orbFrame->SetRadius(sbody->GetMaxChildOrbitalDistance()*1.1);
		b->SetFrame(orbFrame);
		return orbFrame;
	}
	else if (sbody->type == SBody::TYPE_STARPORT_ORBITAL) {
		// space stations want non-rotating frame to some distance
		// and a much closer rotating frame
		frameRadius = 1000000.0; // XXX NFI!
		orbFrame = new Frame(f, sbody->name.c_str());
		orbFrame->m_sbody = sbody;
		orbFrame->SetRadius(frameRadius ? frameRadius : 10*sbody->GetRadius());
	
		assert(sbody->GetRotationPeriod() != 0);
		rotFrame = new Frame(orbFrame, sbody->name.c_str());
		rotFrame->SetRadius(5000.0);//(1.1*sbody->GetRadius());
		rotFrame->SetAngVelocity(vector3d(0,2*M_PI/sbody->GetRotationPeriod(),0));
		b->SetFrame(rotFrame);
		return orbFrame;
	} else if (sbody->type == SBody::TYPE_STARPORT_SURFACE) {
		// just put body into rotating frame of planet, not in its own frame
		// (because collisions only happen between objects in same frame,
		// and we want collisions on starport and on planet itself)
		Frame *frame = *f->m_children.begin();
		b->SetFrame(frame);
		assert(frame->m_astroBody->IsType(Object::PLANET));
		Planet *planet = static_cast<Planet*>(frame->m_astroBody);

		/* position on planet surface */
		double height;
		int tries;
		matrix4x4d rot;
		vector3d pos;
		// first try suggested position
		rot = sbody->orbit.rotMatrix;
		pos = rot * vector3d(0,1,0);
		if (planet->GetTerrainHeight(pos) - planet->GetRadius() == 0.0) {
			MTRand r(sbody->seed);
			// position is under water. try some random ones
			for (tries=0; tries<100; tries++) {
				// used for orientation on planet surface
				rot = matrix4x4d::RotateZMatrix(2*M_PI*r.Double()) *
						      matrix4x4d::RotateYMatrix(2*M_PI*r.Double());
				pos = rot * vector3d(0,1,0);
				height = planet->GetTerrainHeight(pos) - planet->GetRadius();
				// don't want to be under water
				if (height > 0.0) break;
			}
		}
		b->SetPosition(pos * planet->GetTerrainHeight(pos));
		b->SetRotMatrix(rot);
		return frame;
	} else {
		assert(0);
	}
}

void GenBody(SBody *sbody, Frame *f)
{
	Body *b = 0;

	if (sbody->type != SBody::TYPE_GRAVPOINT) {
		if (sbody->GetSuperType() == SBody::SUPERTYPE_STAR) {
			Star *star = new Star(sbody);
			b = star;
		} else if ((sbody->type == SBody::TYPE_STARPORT_ORBITAL) ||
		           (sbody->type == SBody::TYPE_STARPORT_SURFACE)) {
			SpaceStation *ss = new SpaceStation(sbody);
			b = ss;
		} else {
			Planet *planet = new Planet(sbody);
			b = planet;
		}
		b->SetLabel(sbody->name.c_str());
		b->SetPosition(vector3d(0,0,0));
		AddBody(b);
	}
	f = MakeFrameFor(sbody, b, f);

	for (std::vector<SBody*>::iterator i = sbody->children.begin(); i != sbody->children.end(); ++i) {
		GenBody(*i, f);
	}
}

void BuildSystem()
{
	GenBody(Pi::currentSystem->rootBody, rootFrame);
	MoveOrbitingObjectFrames(rootFrame);
}

void AddBody(Body *b)
{
	bodies.push_back(b);
}

void RemoveBody(Body *b)
{
	b->SetFrame(0);
	bodies.remove(b);
}

void KillBody(Body* const b)
{
	if (!b->IsDead()) {
		b->MarkDead();
		if (b != Pi::player) corpses.push_back(b);
	}
}

void UpdateFramesOfReference()
{
	for (std::list<Body*>::iterator i = bodies.begin(); i != bodies.end(); ++i) {
		Body *b = *i;

		if (!(b->GetFlags() & Body::FLAG_CAN_MOVE_FRAME)) continue;

		// falling out of frames
		if (!b->GetFrame()->IsLocalPosInFrame(b->GetPosition())) {
			printf("%s leaves frame %s\n", b->GetLabel().c_str(), b->GetFrame()->GetLabel());
			
			vector3d oldFrameVel = b->GetFrame()->GetVelocity();
			
			Frame *new_frame = b->GetFrame()->m_parent;
			if (new_frame) { // don't let fall out of root frame
				matrix4x4d m = matrix4x4d::Identity();
				b->GetFrame()->ApplyLeavingTransform(m);

				vector3d new_pos = m * b->GetPosition();//b->GetPositionRelTo(new_frame);

				matrix4x4d rot;
				b->GetRotMatrix(rot);
				b->SetRotMatrix(m * rot);
				
				b->SetVelocity(oldFrameVel + m.ApplyRotationOnly(b->GetVelocity() - 
					b->GetFrame()->GetStasisVelocityAtPosition(b->GetPosition())));

				b->SetFrame(new_frame);
				b->SetPosition(new_pos);
			} else {
				b->SetVelocity(b->GetVelocity() + oldFrameVel);
			}
		}

		// entering into frames
		for (std::list<Frame*>::iterator j = b->GetFrame()->m_children.begin(); j != b->GetFrame()->m_children.end(); ++j) {
			Frame *kid = *j;
			matrix4x4d m;
			Frame::GetFrameTransform(b->GetFrame(), kid, m);
			vector3d pos = m * b->GetPosition();
			if (kid->IsLocalPosInFrame(pos)) {
				printf("%s enters frame %s\n", b->GetLabel().c_str(), kid->GetLabel());
				b->SetPosition(pos);
				b->SetFrame(kid);

				matrix4x4d rot;
				b->GetRotMatrix(rot);
				b->SetRotMatrix(m * rot);
				
				// get rid of transforms
				m.ClearToRotOnly();
				b->SetVelocity(m*b->GetVelocity()
					- kid->GetVelocity()
					+ kid->GetStasisVelocityAtPosition(pos));
				break;
			}
		}
	}
}

static bool OnCollision(Object *o1, Object *o2, CollisionContact *c, double relativeVel)
{
	Body *pb1 = static_cast<Body*>(o1);
	Body *pb2 = static_cast<Body*>(o2);
	/* Not always a Body (could be CityOnPlanet, which is a nasty exception I should eradicate) */
	if (o1->IsType(Object::BODY)) {
		if (pb1 && !pb1->OnCollision(o2, c->geomFlag, relativeVel)) return false;
	}
	if (o2->IsType(Object::BODY)) {
		if (pb2 && !pb2->OnCollision(o1, c->geomFlag, relativeVel)) return false;
	}
	return true;
}

static void hitCallback(CollisionContact *c)
{
	//printf("OUCH! %x (depth %f)\n", SDL_GetTicks(), c->depth);

	Object *po1 = static_cast<Object*>(c->userData1);
	Object *po2 = static_cast<Object*>(c->userData2);

	const bool po1_isDynBody = po1->IsType(Object::DYNAMICBODY);
	const bool po2_isDynBody = po2->IsType(Object::DYNAMICBODY);
	// collision response
	assert(po1_isDynBody || po2_isDynBody);

	if (po1_isDynBody && po2_isDynBody) {
		DynamicBody *b1 = static_cast<DynamicBody*>(po1);
		DynamicBody *b2 = static_cast<DynamicBody*>(po2);
		const vector3d linVel1 = b1->GetVelocity();
		const vector3d linVel2 = b2->GetVelocity();
		const vector3d angVel1 = b1->GetAngVelocity();
		const vector3d angVel2 = b2->GetAngVelocity();
		
		const double coeff_rest = 0.5;
		// step back
//		mover->UndoTimestep();

		const double invMass1 = 1.0 / b1->GetMass();
		const double invMass2 = 1.0 / b2->GetMass();
		const vector3d hitPos1 = c->pos - b1->GetPosition();
		const vector3d hitPos2 = c->pos - b2->GetPosition();
		const vector3d hitVel1 = linVel1 + vector3d::Cross(angVel1, hitPos1);
		const vector3d hitVel2 = linVel2 + vector3d::Cross(angVel2, hitPos2);
		const double relVel = vector3d::Dot(hitVel1 - hitVel2, c->normal);
		// moving away so no collision
		if (relVel > 0) return;
		if (!OnCollision(po1, po2, c, -relVel)) return;
		const double invAngInert1 = 1.0 / b1->GetAngularInertia();
		const double invAngInert2 = 1.0 / b2->GetAngularInertia();
		const double numerator = -(1.0 + coeff_rest) * relVel;
		const double term1 = invMass1;
		const double term2 = invMass2;
		const double term3 = vector3d::Dot(c->normal, vector3d::Cross(vector3d::Cross(hitPos1, c->normal)*invAngInert1, hitPos1));
		const double term4 = vector3d::Dot(c->normal, vector3d::Cross(vector3d::Cross(hitPos2, c->normal)*invAngInert2, hitPos2));

		const double j = numerator / (term1 + term2 + term3 + term4);
		const vector3d force = j * c->normal;
					
		b1->SetVelocity(linVel1 + force*invMass1);
		b1->SetAngVelocity(angVel1 + vector3d::Cross(hitPos1, force)*invAngInert1);
		b2->SetVelocity(linVel2 - force*invMass2);
		b2->SetAngVelocity(angVel2 - vector3d::Cross(hitPos2, force)*invAngInert2);
	} else {
		// one body is static
		vector3d hitNormal;
		DynamicBody *mover;

		if (po1_isDynBody) {
			mover = static_cast<DynamicBody*>(po1);
			hitNormal = c->normal;
		} else {
			mover = static_cast<DynamicBody*>(po2);
			hitNormal = -c->normal;
		}

		const double coeff_rest = 0.5;
		const vector3d linVel1 = mover->GetVelocity();
		const vector3d angVel1 = mover->GetAngVelocity();
		
		// step back
//		mover->UndoTimestep();

		const double invMass1 = 1.0 / mover->GetMass();
		const vector3d hitPos1 = c->pos - mover->GetPosition();
		const vector3d hitVel1 = linVel1 + vector3d::Cross(angVel1, hitPos1);
		const double relVel = vector3d::Dot(hitVel1, c->normal);
		// moving away so no collision
		if (relVel > 0) return;
		if (!OnCollision(po1, po2, c, -relVel)) return;
		const double invAngInert = 1.0 / mover->GetAngularInertia();
		const double numerator = -(1.0 + coeff_rest) * relVel;
		const double term1 = invMass1;
		const double term3 = vector3d::Dot(c->normal, vector3d::Cross(vector3d::Cross(hitPos1, c->normal)*invAngInert, hitPos1));

		const double j = numerator / (term1 + term3);
		const vector3d force = j * c->normal;
					
		mover->SetVelocity(linVel1 + force*invMass1);
		mover->SetAngVelocity(angVel1 + vector3d::Cross(hitPos1, force)*invAngInert);
	}
}

void CollideFrame(Frame *f)
{
	if (f->m_astroBody && (f->m_astroBody->IsType(Object::PLANET))) {
		// this is pretty retarded
		for (bodiesIter_t i = bodies.begin(); i!=bodies.end(); ++i) {
			if ((*i)->GetFrame() != f) continue;
			if (!(*i)->IsType(Object::DYNAMICBODY)) continue;
			DynamicBody *dynBody = (DynamicBody*)(*i);

			Aabb aabb;
			dynBody->GetAabb(aabb);
			const matrix4x4d &trans = dynBody->GetGeom()->GetTransform();

			const vector3d aabbCorners[8] = {
				vector3d(aabb.min.x, aabb.min.y, aabb.min.z),
				vector3d(aabb.min.x, aabb.min.y, aabb.max.z),
				vector3d(aabb.min.x, aabb.max.y, aabb.min.z),
				vector3d(aabb.min.x, aabb.max.y, aabb.max.z),
				vector3d(aabb.max.x, aabb.min.y, aabb.min.z),
				vector3d(aabb.max.x, aabb.min.y, aabb.max.z),
				vector3d(aabb.max.x, aabb.max.y, aabb.min.z),
				vector3d(aabb.max.x, aabb.max.y, aabb.max.z)
			};

			CollisionContact c;

			for (int i=0; i<8; i++) {
				const vector3d &s = aabbCorners[i];
				vector3d pos = trans * s;
				double terrain_height = static_cast<Planet*>(f->m_astroBody)->GetTerrainHeight(pos.Normalized());
				double altitude = pos.Length();
				double hitDepth = terrain_height - altitude;
				if (altitude < terrain_height) {
					c.pos = pos;
					c.normal = pos.Normalized();
					c.depth = hitDepth;
					c.userData1 = static_cast<void*>(dynBody);
					c.userData2 = static_cast<void*>(f->m_astroBody);
					hitCallback(&c);
				}
			}
		}
	}
	f->GetCollisionSpace()->Collide(&hitCallback);
	for (std::list<Frame*>::iterator i = f->m_children.begin(); i != f->m_children.end(); ++i) {
		CollideFrame(*i);
	}
}

void ApplyGravity()
{
	Body *lump = 0;
	// gravity is applied when our frame contains an 'astroBody', ie a star or planet,
	// or when our frame contains a rotating frame which contains this body.
	if (Pi::player->GetFrame()->m_astroBody) {
		lump = Pi::player->GetFrame()->m_astroBody;
	} else if (Pi::player->GetFrame()->m_sbody &&
		(Pi::player->GetFrame()->m_children.begin() !=
	           Pi::player->GetFrame()->m_children.end())) {

		lump = (*Pi::player->GetFrame()->m_children.begin())->m_astroBody;
	}
	// just to crap in the player's frame
	if (lump) { 
		for (std::list<Body*>::iterator i = bodies.begin(); i != bodies.end(); ++i) {
			if ((*i)->GetFrame() != Pi::player->GetFrame()) continue;
			if (!(*i)->IsType(Object::DYNAMICBODY)) continue;

			vector3d b1b2 = lump->GetPosition() - (*i)->GetPosition();
			const double m1m2 = (*i)->GetMass() * lump->GetMass();
			const double r = b1b2.Length();
			const double force = G*m1m2 / (r*r);
			b1b2 = b1b2.Normalized() * force;
			static_cast<DynamicBody*>(*i)->AddForce(b1b2);
		}
	}

}

void TimeStep(float step)
{
	if (hyperspacingTo) {
		hyperspaceAnim += step;
		if (hyperspaceAnim > 1.0) {
			DoHyperspaceTo(0);
			hyperspaceAnim = 0;
		}
	}

	ApplyGravity();
	CollideFrame(rootFrame);
	// XXX does not need to be done this often
	UpdateFramesOfReference();
	MoveOrbitingObjectFrames(rootFrame);
	
	for (bodiesIter_t i = bodies.begin(); i != bodies.end(); ++i) {
		(*i)->TimeStepUpdate(step);
	}
	for (bodiesIter_t i = bodies.begin(); i != bodies.end(); ++i) {
		(*i)->StaticUpdate(step);
	}
	Sfx::TimeStepAll(step, rootFrame);
	// see if anyone has been shot

	PruneCorpses();
}

void PruneCorpses()
{
	for (bodiesIter_t corpse = corpses.begin(); corpse != corpses.end(); ++corpse) {
		for (bodiesIter_t i = bodies.begin(); i != bodies.end(); ++i)
			(*i)->NotifyDeath(*corpse);
		bodies.remove(*corpse);
		delete *corpse;
	}
	corpses.clear();
}

static bool jumped_within_same_system;

/*
 * Called during play to initiate hyperspace sequence.
 */
void StartHyperspaceTo(const SBodyPath *dest)
{
	int fuelUsage;
	if (!Pi::player->CanHyperspaceTo(dest, fuelUsage)) return;
	if (Pi::currentSystem->IsSystem(dest->sectorX, dest->sectorY, dest->systemIdx)) {
		return;
	}
	
	Space::Clear();
	Pi::player->UseHyperspaceFuel(dest);
	Pi::player->DisableBodyOnly();
	
	if (!hyperspacingTo) hyperspacingTo = new SBodyPath;
	*hyperspacingTo = *dest;
	hyperspaceAnim = 0.0f;
	printf("Started hyperspacing...\n");
}

/*
 * Called at end of hyperspace sequence or at start of game
 * to place the player in a system.
 */
void DoHyperspaceTo(const SBodyPath *dest)
{
	if (dest == 0) dest = hyperspacingTo;
	
	if (Pi::currentSystem) delete Pi::currentSystem;
	Pi::currentSystem = new StarSystem(dest->sectorX, dest->sectorY, dest->systemIdx);
	Space::Clear();
	Space::BuildSystem();
	SBody *targetBody = Pi::currentSystem->GetBodyByPath(dest);
	Frame *pframe = Space::GetFrameWithSBody(targetBody);
	assert(pframe);
	float longitude = Pi::rng.Double(M_PI);
	float latitude = Pi::rng.Double(M_PI);
	float dist = (0.4 + Pi::rng.Double(0.2)) * AU;
	Pi::player->SetPosition(vector3d(sin(longitude)*cos(latitude)*dist,
			sin(latitude)*dist,
			cos(longitude)*cos(latitude)*dist));
	Pi::player->SetVelocity(vector3d(0.0));
	Pi::player->SetFrame(pframe);
	Pi::player->Enable();

	Pi::onPlayerHyperspaceToNewSystem.emit();
	SpawnPiratesOnHyperspace();
	
	delete hyperspacingTo;
	hyperspacingTo = 0;
}

float GetHyperspaceAnim()
{
	return hyperspaceAnim;
}

struct body_zsort_t {
	double dist;
	Body *b;
};

struct body_zsort_compare : public std::binary_function<body_zsort_t, body_zsort_t, bool> {
	bool operator()(body_zsort_t a, body_zsort_t b) { return a.dist > b.dist; }
};

void Render(const Frame *cam_frame)
{
	// simple z-sort!!!!!!!!!!!!!11
	body_zsort_t *bz = new body_zsort_t[bodies.size()];
	int idx = 0;
	for (std::list<Body*>::iterator i = bodies.begin(); i != bodies.end(); ++i) {
		vector3d toBody = (*i)->GetPositionRelTo(cam_frame);
		bz[idx].dist = toBody.Length();
		bz[idx].b = *i;
		idx++;
	}
	sort(bz, bz+bodies.size(), body_zsort_compare());

	// Probably the right place for this when partitioning is done
	sbreSetDepthRange (Pi::GetScrWidth()*0.5f, 0.0f, 1.0f);

	for (unsigned int i=0; i<bodies.size(); i++) {
		bz[i].b->Render(cam_frame);
	}
	Sfx::RenderAll(rootFrame, cam_frame);

	delete [] bz;
}

}

