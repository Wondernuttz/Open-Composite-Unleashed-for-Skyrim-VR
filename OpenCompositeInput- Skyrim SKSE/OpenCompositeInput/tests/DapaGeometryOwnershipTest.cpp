#include "../src/DapaGeometryOwnership.h"
#include <array>
#include <cstdio>
#include <stdexcept>

struct Reference {};
struct Node { const Node* parent=nullptr; const Reference* userData=nullptr; };
static void Check(bool value,const char* reason) { if(!value)throw std::runtime_error(reason); }

int main()
{
    try {
        using namespace DapaGeometryOwnership;
        Reference player,projectile,ui;
        Node world{},playerWorld{&world},first{&world,&player},third{&world,&player};
        Node rightWand{&playerWorld},leftWand{&playerWorld};
        Node arrow{&playerWorld},hold{&rightWand},snap{&leftWand},fire{&playerWorld};
        Node arrowMesh{&arrow},heldMesh{&hold},nockedMesh{&snap},fireMesh{&fire};
        Node hand{&first},armor{&third},heldObject{&world},heldObjectMesh{&heldObject};
        Roots<Node> roots{&first,&third,&arrow,&hold,&snap};
        const Reference* nearest=nullptr;
        const auto heldMatch=[&](const Node* node){return node==&heldObject;};
        const auto classify=[&](const Node* node){return Classify(node,roots,heldMatch,nearest);};
        Check(classify(&hand)==Kind::Player && classify(&armor)==Kind::Player,"both body hierarchies remain covered");
        Check(classify(&heldObjectMesh)==Kind::Held,"HIGGS held-object hierarchy remains covered");
        // Native offset containers are also siblings of the body skeleton.
        // The general weapon offset is not an ancestor of the crossbow offset.
        std::array<Node,11> gear{};
        for(unsigned i=0;i<gear.size();++i) {
            gear[i].parent=i%2?&leftWand:&rightWand;
            roots.equipment[i]=&gear[i];
            Node mesh{&gear[i]};
            Check(classify(&mesh)==Kind::AttachedEquipment,"every native left/right equipment attachment is covered");
            mesh.parent=&world;
            Check(classify(&mesh)==Kind::None,"released equipment cannot keep attachment ownership");
        }
        // Live Skyrim snapshot: these native VR containers are siblings of the
        // skeleton, not descendants. None has a player userData pointer.
        Check(classify(&arrowMesh)==Kind::AttachedArrow,"separate native arrow container");
        Check(classify(&heldMesh)==Kind::AttachedArrow,"arrow in wand hold container");
        Check(classify(&nockedMesh)==Kind::AttachedArrow,"arrow in bow snap container");
        Check(classify(&fireMesh)==Kind::None,"launch container must not freeze world projectiles");
        Check(classify(&playerWorld)==Kind::None && classify(&rightWand)==Kind::None,"shared VR/world ancestors excluded");
        Node replacementGear{&leftWand},oldGearMesh{&gear[0]};
        roots.equipment[0]=&replacementGear;
        Check(classify(&oldGearMesh)==Kind::None,"replaced native equipment root is not retained");
        nockedMesh.parent=&world;nockedMesh.userData=&projectile;
        Check(classify(&nockedMesh)==Kind::None && nearest==&projectile,"detached fired mesh loses attached ownership immediately");
        Node replacement{&playerWorld};
        roots.arrow=&replacement;
        Check(classify(&arrowMesh)==Kind::None,"old ammo root is not retained after replacement");
        arrowMesh.parent=&replacement;
        Check(classify(&arrowMesh)==Kind::AttachedArrow,"replacement ammo uses current root");
        roots.arrowHold=nullptr;roots.arrowSnap=nullptr;
        Check(classify(&heldMesh)==Kind::None && classify(&nockedMesh)==Kind::None,"missing roots cannot preserve old ownership");
        Node uiOwner{&world,&ui},uiMesh{&uiOwner,&projectile};
        Check(classify(&uiMesh)==Kind::None && nearest==&projectile,"nearest reference takes precedence for downstream Spell Wheel ownership");
        uiMesh.userData=nullptr;
        Check(classify(&uiMesh)==Kind::None && nearest==&ui,"unowned child resolves nearest parent reference");
        std::array<Node,65> deep{};
        for(unsigned i=0;i<64;++i)deep[i].parent=&deep[i+1];
        deep[64].parent=&first;
        Check(classify(&deep[0])==Kind::None,"walk is bounded on unexpectedly deep geometry");
        Node cycle{};cycle.parent=&cycle;
        Check(classify(&cycle)==Kind::None,"cyclic ancestry terminates");
        Check(classify(nullptr)==Kind::None && nearest==nullptr,"null geometry clears reference output");
        std::puts("PASS: native VR arrow hierarchy, body/HIGGS coverage, detach on fire, ammo replacement, nearest owner, null/deep/cyclic ancestry");
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what());return 1; }
}
