#pragma once
// Starting points for a new project, and the demo song. All of them are built in code from the
// built-in instruments and the factory library ("factory:" paths), so they need no files of their own.
#include "pulp/Project.h"

namespace pulp {

struct TemplateInfo {
    std::string name, description;
};
std::vector<TemplateInfo> projectTemplates();
// Fills a fresh Project with template number `index`. The factory folder must be set on the SampleCache;
// an instrument whose factory files are missing is replaced by a synthesizer.
bool makeTemplate(Project& p, int index);
// A complete short song: seven instruments, drums, a bass that ducks under the kick (sidechain), a reverb
// bus, a delay, automation clips, and an arrangement in the playlist. It doubles as an end-to-end test.
void makeDemoSong(Project& p);

} // namespace pulp
