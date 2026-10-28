// Moved to tools/merge.cpp.
//
// The recursive src/*.cpp glob picks this file up in every target, so a
// main() here collided with each tool's own. Kept as a deliberately empty
// translation unit rather than deleted: the source lists and the build cache
// stay valid, nothing links, and the note tells the next reader where the
// entry point went.
