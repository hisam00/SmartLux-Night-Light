// register.js
import { auth, db, createUserWithEmailAndPassword, doc, setDoc, sendEmailVerification } from './firebase-config.js';

const registerForm = document.getElementById('registerForm');

registerForm.addEventListener('submit', async (e) => {
  e.preventDefault();

  const firstName = document.getElementById('firstName').value.trim();
  const lastName = document.getElementById('lastName').value.trim();
  const email = document.getElementById('email').value.trim();
  const username = document.getElementById('username').value.trim();
  const password = document.getElementById('password').value;
  const repeatPassword = document.getElementById('repeatPassword').value;

  if (password !== repeatPassword) {
    alert("Passwords do not match!");
    return;
  }

  try {
    const userCredential = await createUserWithEmailAndPassword(auth, email, password);
    const user = userCredential.user;

    // Add user data with role to Firestore
    await setDoc(doc(db, "users", user.uid), {
      firstName,
      lastName,
      email,
      username,
      role: "user",         // <- Add default role here
      createdAt: new Date()
    });

    // Send verification email
    await sendEmailVerification(user);
    console.log('Verification email sent to', user.email);
    alert('Registration successful! Verification email sent. You will be redirected.');
    fetch("https://smartlux-night-light-backend.onrender.com/api/syncUserCount").catch(err => console.error('Failed to sync user count:', err));
    location.replace('dashboard.html');
  } catch (error) {
    console.error(error);
    alert("Error: " + error.message);
  } finally {
    registerForm.reset();
  }
});
