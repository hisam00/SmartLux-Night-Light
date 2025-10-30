const admin = require('firebase-admin');
const serviceAccount = require('./serviceAccountKey.json');

admin.initializeApp({
  credential: admin.credential.cert(serviceAccount),
  databaseURL: "https://smartlux-night-light-default-rtdb.asia-southeast1.firebasedatabase.app"
});

const db = admin.firestore();

async function addAvatarUrlToAllUsers() {
  const usersCol = db.collection('users');
  try {
    const snapshot = await usersCol.get();
    if (snapshot.empty) {
      console.log("No user documents found.");
      return;
    }

    let batch = db.batch();
    const batchSize = 500;
    let batchCounter = 0;
    const batchCommits = [];

    for (const docSnap of snapshot.docs) {
      const data = docSnap.data();
      if (!data.avatarUrl) {
        const userRef = usersCol.doc(docSnap.id);
        batch.update(userRef, { avatarUrl: '' });  // or provide default placeholder URL if preferred
        batchCounter++;

        if (batchCounter === batchSize) {
          batchCommits.push(batch.commit().catch(err => {
            console.error("Error committing batch:", err);
            throw err;  // re-throw to abort if needed
          }));
          batch = db.batch();
          batchCounter = 0;
        }
      }
    }

    if (batchCounter > 0) {
      batchCommits.push(batch.commit().catch(err => {
        console.error("Error committing final batch:", err);
        throw err;
      }));
    }

    await Promise.all(batchCommits);
    console.log(`Batch update complete! Processed ${snapshot.size} user documents.`);
  } catch (err) {
    console.error("Failed to update avatarUrl for users:", err);
  }
}

addAvatarUrlToAllUsers()
  .then(() => {
    console.log("Script finished successfully.");
    process.exit(0);
  })
  .catch((err) => {
    console.error("Script failed:", err);
    process.exit(1);
  });
