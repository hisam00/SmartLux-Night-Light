// server.js
const express = require('express');
const cors = require('cors');
const admin = require('firebase-admin');

const app = express();
app.use(cors());
app.use(express.json());

const serviceAccount = require('./serviceAccountKey.json');

admin.initializeApp({
  credential: admin.credential.cert(serviceAccount),
});

const auth = admin.auth();
const firestore = admin.firestore();

// API: Create new user
app.post('/api/createUser', async (req, res) => {
  try {
    const { email, password, username, firstName, lastName, role } = req.body;
    if (!email || !password || !username) {
      return res.status(400).json({ error: "Missing fields." });
    }
    const userRecord = await auth.createUser({ email, password, displayName: username });
    await firestore.collection('users').doc(userRecord.uid).set({
      username, firstName, lastName, email, role, createdAt: admin.firestore.FieldValue.serverTimestamp(),
    });
    res.status(201).json({ uid: userRecord.uid, message: "User created!" });
  } catch (error) {
    console.error("Create User Error:", error);
    res.status(500).json({ error: error.message });
  }
});

// API: Update existing user's Auth email/displayName and Firestore user doc
app.post('/api/updateUser', async (req, res) => {
  try {
    const { uid, email, username, firstName, lastName, role } = req.body;
    if (!uid || !email || !username) {
      return res.status(400).json({ error: 'Missing required fields (uid, email, username).' });
    }

    // 1) Update Firebase Auth
    try {
      await auth.updateUser(uid, { email, displayName: username });
    } catch (err) {
      console.error('Auth update error for uid', uid, err);
      return res.status(500).json({ error: 'Failed to update Firebase Auth user: ' + err.message });
    }

    // 2) Update Firestore users collection
    const userRef = firestore.collection('users').doc(uid);
    await userRef.update({
      username,
      firstName,
      lastName,
      email,
      role,
      updatedAt: admin.firestore.FieldValue.serverTimestamp(),
    });

    res.status(200).json({ message: 'User updated successfully.' });
  } catch (error) {
    console.error("Update User Error:", error);
    res.status(500).json({ error: error.message });
  }
});

// API: Update existing user's password securely
app.post('/api/updateUserPassword', async (req, res) => {
  try {
    const { uid, newPassword } = req.body;
    if (!uid || !newPassword) {
      return res.status(400).json({ error: 'Missing user ID or new password.' });
    }
    await auth.updateUser(uid, { password: newPassword });
    res.status(200).json({ message: 'Password updated successfully.' });
  } catch (error) {
    console.error("Update Password Error:", error);
    res.status(500).json({ error: error.message });
  }
});

// NEW API: Delete user from Firebase Auth and Firestore
app.post('/api/deleteUser', async (req, res) => {
  try {
    const { uid } = req.body;
    if (!uid) {
      return res.status(400).json({ error: 'Missing uid.' });
    }

    // Attempt to delete from Firebase Auth
    let authDeleted = false;
    try {
      await auth.deleteUser(uid);
      authDeleted = true;
    } catch (authErr) {
      // If the user doesn't exist or other error, log it but continue to try Firestore delete.
      console.error(`Failed to delete Auth user ${uid}:`, authErr);
      // If the error is something other than 'user-not-found', we return an error so admin can investigate.
      if (authErr.code && authErr.code !== 'auth/user-not-found') {
        return res.status(500).json({ error: 'Failed to delete user in Firebase Auth: ' + authErr.message });
      }
      // if user-not-found, proceed to delete Firestore doc anyway
    }

    // Delete from Firestore
    try {
      await firestore.collection('users').doc(uid).delete();
    } catch (fsErr) {
      console.error(`Failed to delete Firestore doc for ${uid}:`, fsErr);
      // If Auth deletion succeeded but Firestore failed, respond with partial failure info
      if (authDeleted) {
        return res.status(500).json({ error: 'Auth user deleted but failed to delete Firestore doc: ' + fsErr.message });
      }
      return res.status(500).json({ error: 'Failed to delete Firestore doc: ' + fsErr.message });
    }

    res.status(200).json({ message: 'User deleted from Auth and Firestore (if present).' });
  } catch (error) {
    console.error("Delete User Error:", error);
    res.status(500).json({ error: error.message });
  }
});

// Basic root route for confirmation
app.get('/', (req, res) => {
  res.send('Backend server is running.');
});

// Add near other endpoints in server.js

// 1) Check user existence (admin lookup)
app.post('/api/checkUserByEmail', async (req, res) => {
  const { email } = req.body || {};
  if (!email) return res.status(400).json({ ok: false, error: 'email required' });
  try {
    const user = await auth.getUserByEmail(email);
    return res.json({
      ok: true,
      uid: user.uid,
      email: user.email,
      providers: (user.providerData || []).map(p => p.providerId),
    });
  } catch (err) {
    if (err.code === 'auth/user-not-found') {
      return res.json({ ok: false, code: 'user-not-found' });
    }
    console.error('Admin lookup error:', err);
    return res.status(500).json({ ok: false, error: err.message || err });
  }
});

// 2) Generate password reset link (and optionally send email)
// NOTE: In production you should send the link via your own email provider.
// For testing you can return the link in JSON (NOT recommended in production).
app.post('/api/sendResetLink', async (req, res) => {
  const { email, sendEmail } = req.body || {};
  if (!email) return res.status(400).json({ ok: false, error: 'email required' });

  try {
    // 1) Confirm user exists (throws if not)
    const user = await auth.getUserByEmail(email);

    // 2) Ensure user supports password sign-in
    const providers = (user.providerData || []).map(p => p.providerId);
    if (!providers.includes('password')) {
      // For security, you may want to respond with a generic message instead.
      return res.json({ ok: false, code: 'no-password-provider', providers });
    }

    // 3) Generate password reset link
    const link = await auth.generatePasswordResetLink(email);

    // Option A: return the link (for debugging/testing only)
    if (!sendEmail) {
      return res.json({ ok: true, uid: user.uid, link });
    }

  } catch (err) {
    console.error('sendResetLink error:', err);
    if (err.code === 'auth/user-not-found') {
      // For debugging we return code. In production, consider returning generic success.
      return res.json({ ok: false, code: 'user-not-found' });
    }
    return res.status(500).json({ ok: false, error: err.message || err });
  }
});


const PORT = 3000;
app.listen(PORT, () => console.log(`Admin backend running at http://localhost:${PORT}`));
