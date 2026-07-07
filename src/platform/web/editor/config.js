// Shared page context: the URL query and the scene it selects. A leaf
// module (no imports) so every other module can read these during its own
// evaluation without ordering concerns.

export const query = new URLSearchParams(location.search);
export const sceneName = query.get('scene') || 'plasma'; // the runtime's default
