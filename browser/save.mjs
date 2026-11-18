export async function openEdits(core, key, onError) {
  const db = await new Promise((resolve, reject) => {
    const request = indexedDB.open('strata-preview', 1);
    request.onupgradeneeded = () => request.result.createObjectStore('edits');
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });

  const saved = await new Promise((resolve, reject) => {
    const request = db.transaction('edits').objectStore('edits').get(key);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
  if (saved instanceof Int32Array && saved.length % 4 === 0 && saved.length <= 4096 * 4) {
    for (let i = 0; i < saved.length; i += 4)
      core.restore_edit(saved[i], saved[i + 1], saved[i + 2], saved[i + 3]);
  }

  let queued = false;
  // Run the snapshot and write in a separate task. IndexedDB orders
  // transactions on the same store, including successive edit snapshots.
  return () => {
    if (queued) return;
    queued = true;
    setTimeout(() => {
      queued = false;
      try {
        const view = new Int32Array(core.memory.buffer, core.edit_words(), core.edit_count() * 4);
        const transaction = db.transaction('edits', 'readwrite');
        transaction.onabort = () => onError(transaction.error);
        transaction.objectStore('edits').put(view.slice(), key);
      } catch (error) {
        onError(error);
      }
    }, 0);
  };
}
