#include "OpenAL.hh"
#include "OpenALAPI.hh"

using namespace gazebo;

////////////////////////////////////////////////////////////////////////////////
/// Constructor
OpenALAPI::OpenALAPI()
{
}

////////////////////////////////////////////////////////////////////////////////
/// Destructor
OpenALAPI::~OpenALAPI()
{
}

////////////////////////////////////////////////////////////////////////////////
// Load
void OpenALAPI::Load(XMLConfigNode *node)
{
#ifdef HAVE_OPENAL
  OpenAL::Instance()->Load( node );
#endif
}

////////////////////////////////////////////////////////////////////////////////
/// Initialize
void OpenALAPI::Init()
{
#ifdef HAVE_OPENAL
  OpenAL::Instance()->Init( );
#endif
}

////////////////////////////////////////////////////////////////////////////////
/// Finalize
void OpenALAPI::Fini()
{
#ifdef HAVE_OPENAL
  OpenAL::Instance()->Fini( );
#endif
}
